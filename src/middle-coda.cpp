/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "middle-coda.hpp"

#include "coda-build.hpp"
#include "coda-members.hpp"
#include "coda-store.hpp"
#include "db-copy.hpp"
#include "format.hpp"
#include "idlist.hpp"
#include "logging.hpp"
#include "options.hpp"
#include "osmdata.hpp"
#include "output-requirements.hpp"
#include "output.hpp"
#include "util.hpp"

#include <osmium/builder/osm_object_builder.hpp>
#include <osmium/osm.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <type_traits>

using coda::N2W_SHIFT;
using coda::NODE_SHIFT;
using coda::NUM_DBS;
using coda::REL_SHIFT;
using coda::WAY_SHIFT;
using coda::X2R_SHIFT;
using coda::db_t;
using coda::decode_nodes;
using coda::decode_pairs;
using coda::decode_relations;
using coda::decode_ways;
using coda::encode_nodes;
using coda::encode_pairs;
using coda::encode_relations;
using coda::encode_ways;
using coda::find_by_id;
using coda::get_tags;
using coda::id_type;
using coda::index_shift;
using coda::member_buckets_t;
using coda::member_type;
using coda::node_t;
using coda::pairs_t;
using coda::relation_t;
using coda::store_t;
using coda::tag_dict_t;
using coda::tags_t;
using coda::txn_t;
using coda::way_t;

namespace {

/// Number of threads reading blocks ahead of an update.
constexpr unsigned PREFETCH_THREADS = 16;

/// Decoded blocks a query instance keeps (per block type).
constexpr std::size_t QUERY_CACHE_BLOCKS = 64;

using clock_type = std::chrono::steady_clock;

std::chrono::microseconds elapsed(clock_type::time_point start)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        clock_type::now() - start);
}

id_type to_id(osmid_t id)
{
    if (id <= 0) {
        throw fmt_error("The CODA middle only supports positive ids ({}).", id);
    }
    return static_cast<id_type>(id);
}

osmium::item_type item_type(std::uint8_t type) noexcept
{
    return type == 0   ? osmium::item_type::node
           : type == 1 ? osmium::item_type::way
                       : osmium::item_type::relation;
}

template <typename BUILDER>
void add_tags(osmium::memory::Buffer *buffer, BUILDER *builder,
              tags_t const &tags)
{
    if (tags.empty()) {
        return;
    }
    osmium::builder::TagListBuilder tl{*buffer, builder};
    for (auto const &[key, value] : tags) {
        tl.add_tag(key, value);
    }
}

void build_node(osmium::memory::Buffer *buffer, id_type id,
                osmium::Location location, tags_t const &tags)
{
    {
        osmium::builder::NodeBuilder builder{*buffer};
        builder.set_id(static_cast<osmid_t>(id));
        builder.set_location(location);
        add_tags(buffer, &builder, tags);
    }
    buffer->commit();
}

void build_way(osmium::memory::Buffer *buffer, way_t const &way)
{
    {
        osmium::builder::WayBuilder builder{*buffer};
        builder.set_id(static_cast<osmid_t>(way.id));
        {
            osmium::builder::WayNodeListBuilder wnl{*buffer, &builder};
            for (std::size_t i = 0; i < way.nodes.size(); ++i) {
                wnl.add_node_ref(osmium::NodeRef{
                    static_cast<osmid_t>(way.nodes[i]), way.locations[i]});
            }
        }
        add_tags(buffer, &builder, way.tags);
    }
    buffer->commit();
}

void build_relation(osmium::memory::Buffer *buffer, relation_t const &rel)
{
    {
        osmium::builder::RelationBuilder builder{*buffer};
        builder.set_id(static_cast<osmid_t>(rel.id));
        if (!rel.members.empty()) {
            osmium::builder::RelationMemberListBuilder rml{*buffer, &builder};
            for (auto const &m : rel.members) {
                rml.add_member(item_type(m.type), static_cast<osmid_t>(m.ref),
                               m.role);
            }
        }
        add_tags(buffer, &builder, rel.tags);
    }
    buffer->commit();
}

/// Find the location of a node in the locations of a way.
std::optional<osmium::Location> location_in(way_t const &way, id_type node)
{
    for (std::size_t i = 0; i < way.nodes.size(); ++i) {
        if (way.nodes[i] == node) {
            return way.locations[i];
        }
    }
    return std::nullopt;
}

/// The parents of member in a sorted index block.
template <typename FUNC>
void for_parents(pairs_t const &pairs, id_type member, FUNC const &func)
{
    for (auto it = std::lower_bound(pairs.begin(), pairs.end(),
                                    std::make_pair(member, id_type{0}));
         it != pairs.end() && it->first == member; ++it) {
        func(it->second);
    }
}

/// The blocks of one parent index, decoded when first used.
class index_blocks_t
{
public:
    index_blocks_t(txn_t const &txn, db_t db, tag_dict_t const &dict)
    : m_txn(&txn), m_dict(&dict), m_db(db)
    {
    }

    /**
     * The (member, parent) pairs of the block with member. Only the last
     * block is kept (decoded blocks are large), so look up members in order.
     */
    pairs_t const &block_of(id_type member)
    {
        auto const key = member >> index_shift(m_db);
        if (!m_valid || key != m_key) {
            m_pairs.clear();
            auto const value = m_txn->get(m_db, key);
            if (value.has_value()) {
                m_pairs = decode_pairs(value.value(),
                                       key << index_shift(m_db), *m_dict);
            }
            m_key = key;
            m_valid = true;
        }
        return m_pairs;
    }

private:
    txn_t const *m_txn;
    tag_dict_t const *m_dict;
    db_t m_db;
    pairs_t m_pairs;
    id_type m_key = 0;
    bool m_valid = false;
};

/// The ids sorted (so that index blocks are visited in order).
std::vector<id_type> sorted_ids(idlist_t const &list)
{
    std::vector<id_type> ids;
    ids.reserve(list.size());
    for (auto const id : list) {
        ids.push_back(static_cast<id_type>(id));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// ---------------------------------------------------------------- queries

class middle_query_coda_t : public middle_query_t
{
public:
    explicit middle_query_coda_t(std::shared_ptr<coda_handle_t> handle)
    : m_handle(std::move(handle))
    {
    }

    osmium::Location get_node_location(osmid_t id) const override
    {
        read_scope_t const scope{this};
        return location(to_id(id)).value_or(osmium::Location{});
    }

    std::size_t nodes_get_list(osmium::WayNodeList *nodes) const override
    {
        // Ways from this middle come with their node locations, so only
        // nodes without one are looked up.
        read_scope_t const scope{this};
        std::size_t count = 0;
        for (auto &nr : *nodes) {
            if (!nr.location().valid() && nr.ref() > 0) {
                nr.set_location(location(static_cast<id_type>(nr.ref()))
                                    .value_or(osmium::Location{}));
            }
            if (nr.location().valid()) {
                ++count;
            }
        }
        return count;
    }

    bool node_get(osmid_t id, osmium::memory::Buffer *buffer) const override
    {
        read_scope_t const scope{this};
        auto const nid = to_id(id);
        if (auto const *node = loose(nid)) {
            build_node(buffer, nid, node->location, node->tags);
            return true;
        }
        // an untagged node in a way
        auto const loc = location(nid);
        if (!loc) {
            return false;
        }
        build_node(buffer, nid, *loc, {});
        return true;
    }

    bool way_get(osmid_t id, osmium::memory::Buffer *buffer) const override
    {
        read_scope_t const scope{this};
        auto const *way = find_way(to_id(id));
        if (!way) {
            return false;
        }
        build_way(buffer, *way);
        return true;
    }

    std::size_t
    rel_members_get(osmium::Relation const &rel, osmium::memory::Buffer *buffer,
                    osmium::osm_entity_bits::type types) const override
    {
        read_scope_t const scope{this};
        std::size_t found = 0;
        for (auto const &member : rel.members()) {
            if (member.ref() <= 0 ||
                !(types &
                  osmium::osm_entity_bits::from_item_type(member.type()))) {
                continue;
            }
            auto const id = static_cast<id_type>(member.ref());
            if (member.type() == osmium::item_type::node) {
                // like the pgsql middle, missing nodes are there without
                // a location
                if (auto const *node = loose(id)) {
                    build_node(buffer, id, node->location, node->tags);
                } else {
                    build_node(buffer, id, osmium::Location{}, {});
                }
                ++found;
            } else if (member.type() == osmium::item_type::way) {
                if (auto const *way = find_way(id)) {
                    build_way(buffer, *way);
                    ++found;
                }
            }
        }
        return found;
    }

    bool relation_get(osmid_t id, osmium::memory::Buffer *buffer) const override
    {
        read_scope_t const scope{this};
        auto const rid = to_id(id);
        auto *rels = block(&m_rels, db_t::rels, rid >> REL_SHIFT);
        auto const *rel = find_by_id(rels, rid);
        if (!rel) {
            return false;
        }
        build_relation(buffer, *rel);
        return true;
    }

private:
    /// Keeps a read transaction during a (possibly nested) query.
    class read_scope_t
    {
    public:
        explicit read_scope_t(middle_query_coda_t const *query) : m_query(query)
        {
            if (m_query->m_depth == 0) {
                if (!m_query->m_handle->store) {
                    throw std::runtime_error{"CODA middle is not open."};
                }
                auto const &store = *m_query->m_handle->store;
                if (m_query->m_txn) {
                    m_query->m_txn->renew();
                } else {
                    m_query->m_txn = std::make_unique<txn_t>(store, true);
                }
                if (m_query->m_generation != store.generation()) {
                    m_query->clear_caches();
                    m_query->m_generation = store.generation();
                }
            }
            ++m_query->m_depth;
        }

        ~read_scope_t() noexcept
        {
            if (--m_query->m_depth == 0) {
                // Don't keep an old snapshot: it would stop writers from
                // reusing free pages.
                m_query->m_txn->reset();
            }
        }

        read_scope_t(read_scope_t const &) = delete;
        read_scope_t &operator=(read_scope_t const &) = delete;
        read_scope_t(read_scope_t &&) = delete;
        read_scope_t &operator=(read_scope_t &&) = delete;

    private:
        middle_query_coda_t const *m_query;
    };

    template <typename T>
    using cache_t = std::unordered_map<id_type, std::vector<T>>;

    void clear_caches() const
    {
        m_ways.clear();
        m_nodes.clear();
        m_rels.clear();
        m_n2w.clear();
    }

    template <typename T>
    std::vector<T> *block(cache_t<T> *cache, db_t db, id_type key) const
    {
        auto it = cache->find(key);
        if (it != cache->end()) {
            return &it->second;
        }
        if (cache->size() >= QUERY_CACHE_BLOCKS) {
            cache->clear();
        }
        std::vector<T> objects;
        if (auto const value = m_txn->get(db, key)) {
            auto const &dict = m_handle->store->dict();
            if constexpr (std::is_same_v<T, way_t>) {
                objects = decode_ways(*value, key, dict);
            } else if constexpr (std::is_same_v<T, node_t>) {
                objects = decode_nodes(*value, key, dict);
            } else {
                objects = decode_relations(*value, key, dict);
            }
        }
        return &cache->emplace(key, std::move(objects)).first->second;
    }

    way_t const *find_way(id_type id) const
    {
        return find_by_id(block(&m_ways, db_t::ways, id >> WAY_SHIFT), id);
    }

    node_t const *loose(id_type id) const
    {
        return find_by_id(block(&m_nodes, db_t::nodes, id >> NODE_SHIFT), id);
    }

    pairs_t const &n2w_block(id_type block) const
    {
        auto it = m_n2w.find(block);
        if (it != m_n2w.end()) {
            return it->second;
        }
        if (m_n2w.size() >= QUERY_CACHE_BLOCKS) {
            m_n2w.clear();
        }
        pairs_t pairs;
        if (auto const value = m_txn->get(db_t::n2w, block)) {
            pairs = decode_pairs(*value, block << N2W_SHIFT,
                                 m_handle->store->dict());
        }
        return m_n2w.emplace(block, std::move(pairs)).first->second;
    }

    /// Location of a node: from the loose nodes or from any parent way.
    std::optional<osmium::Location> location(id_type id) const
    {
        if (auto const *node = loose(id)) {
            return node->location;
        }
        std::optional<osmium::Location> result;
        for_parents(n2w_block(id >> N2W_SHIFT), id, [&](id_type parent) {
            if (result && result->valid()) {
                return;
            }
            if (auto const *way = find_way(parent)) {
                if (auto const loc = location_in(*way, id)) {
                    result = loc;
                }
            }
        });
        return result;
    }

    std::shared_ptr<coda_handle_t> m_handle;
    mutable std::unique_ptr<txn_t> m_txn;
    mutable int m_depth = 0;
    mutable std::uint64_t m_generation = ~static_cast<std::uint64_t>(0);
    mutable cache_t<way_t> m_ways;
    mutable cache_t<node_t> m_nodes;
    mutable cache_t<relation_t> m_rels;
    mutable std::unordered_map<id_type, pairs_t> m_n2w;
}; // class middle_query_coda_t

// ---------------------------------------------------------------- updates

/**
 * Fault in, with many threads, every block an update will read, instead of
 * reading them one by one later. Optionally also finds the parent ways (in
 * the node->way blocks among keys) of the given nodes.
 */
void prefetch(store_t const &store, std::vector<std::pair<db_t, id_type>> keys,
              std::vector<id_type> const *nodes = nullptr,
              std::vector<id_type> *parent_ways = nullptr)
{
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::atomic<std::size_t> next{0};
    std::mutex mutex;
    std::exception_ptr error;

    auto work = [&]() {
        try {
            txn_t const txn{store, true};
            std::vector<id_type> found;
            for (std::size_t i = 0; (i = next++) < keys.size();) {
                auto const [db, key] = keys[i];
                auto const value = txn.get(db, key);
                if (!value) {
                    continue;
                }
                // touch every page
                char volatile sink = 0;
                for (std::size_t o = 0; o < value->size(); o += 4096) {
                    sink = static_cast<char>(sink + (*value)[o]);
                }
                if (parent_ways && db == db_t::n2w) {
                    for (auto const &[node, way] :
                         decode_pairs(*value, key << N2W_SHIFT, store.dict())) {
                        if (std::binary_search(nodes->begin(), nodes->end(),
                                               node)) {
                            found.push_back(way);
                        }
                    }
                }
            }
            if (parent_ways) {
                std::lock_guard<std::mutex> const guard{mutex};
                parent_ways->insert(parent_ways->end(), found.begin(),
                                    found.end());
            }
        } catch (...) {
            std::lock_guard<std::mutex> const guard{mutex};
            error = std::current_exception();
        }
    };

    std::vector<std::thread> threads;
    auto const num =
        std::min<std::size_t>(PREFETCH_THREADS, keys.size() / 16 + 1);
    for (std::size_t i = 0; i < num; ++i) {
        threads.emplace_back(work);
    }
    for (auto &thread : threads) {
        thread.join();
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

/// Decoded blocks being changed in a write transaction.
class updater_t
{
public:
    updater_t(txn_t *txn, tag_dict_t const &dict, unsigned threads)
    : m_txn(txn), m_dict(&dict), m_threads(std::max(threads, 1U))
    {
    }

    std::vector<way_t> &way_block(id_type id)
    {
        return block(&m_ways, db_t::ways, id >> WAY_SHIFT);
    }
    std::vector<node_t> &node_block(id_type id)
    {
        return block(&m_nodes, db_t::nodes, id >> NODE_SHIFT);
    }
    std::vector<relation_t> &rel_block(id_type id)
    {
        return block(&m_rels, db_t::rels, id >> REL_SHIFT);
    }

    void way_dirty(id_type id) { m_ways[id >> WAY_SHIFT].dirty = true; }
    void node_dirty(id_type id) { m_nodes[id >> NODE_SHIFT].dirty = true; }
    void rel_dirty(id_type id) { m_rels[id >> REL_SHIFT].dirty = true; }

    way_t *way(id_type id) { return find_by_id(&way_block(id), id); }
    node_t *loose(id_type id) { return find_by_id(&node_block(id), id); }
    relation_t *rel(id_type id) { return find_by_id(&rel_block(id), id); }

    template <typename T>
    static void upsert(std::vector<T> *objects, T object)
    {
        auto const it = std::lower_bound(
            objects->begin(), objects->end(), object.id,
            [](T const &o, id_type i) noexcept { return o.id < i; });
        if (it != objects->end() && it->id == object.id) {
            *it = std::move(object);
        } else {
            objects->insert(it, std::move(object));
        }
    }

    template <typename T>
    static bool erase(std::vector<T> *objects, id_type id)
    {
        auto const it = std::lower_bound(
            objects->begin(), objects->end(), id,
            [](T const &o, id_type i) noexcept { return o.id < i; });
        if (it != objects->end() && it->id == id) {
            objects->erase(it);
            return true;
        }
        return false;
    }

    void upsert_loose(node_t &&node)
    {
        auto const id = node.id;
        upsert(&node_block(id), std::move(node));
        node_dirty(id);
    }

    void erase_loose(id_type id)
    {
        if (erase(&node_block(id), id)) {
            node_dirty(id);
        }
    }

    /// Parents of a member (sorted).
    std::vector<id_type> parents(db_t db, id_type member)
    {
        std::vector<id_type> result;
        auto &b = index_block(db, member);
        for_parents(b.pairs, member, [&](id_type parent) {
            if (b.removed.empty() || !b.removed.count({member, parent})) {
                result.push_back(parent);
            }
        });
        auto it = b.added.lower_bound({member, 0});
        if (it != b.added.end() && it->first == member) {
            auto const stored = result.size();
            for (; it != b.added.end() && it->first == member; ++it) {
                result.push_back(it->second);
            }
            std::inplace_merge(
                result.begin(),
                result.begin() + static_cast<std::ptrdiff_t>(stored),
                result.end());
        }
        return result;
    }

    bool has_parent(db_t db, id_type member)
    {
        auto &b = index_block(db, member);
        auto const it = b.added.lower_bound({member, 0});
        if (it != b.added.end() && it->first == member) {
            return true;
        }
        bool found = false;
        for_parents(b.pairs, member, [&](id_type parent) {
            found = found || !b.removed.count({member, parent});
        });
        return found;
    }

    void add_pair(db_t db, id_type member, id_type parent)
    {
        auto &b = index_block(db, member);
        std::pair<id_type, id_type> const pair{member, parent};
        if (b.removed.erase(pair) > 0) {
            b.dirty = true;
        } else if (!std::binary_search(b.pairs.begin(), b.pairs.end(),
                                       pair)) {
            b.dirty |= b.added.insert(pair).second;
        }
    }

    void del_pair(db_t db, id_type member, id_type parent)
    {
        auto &b = index_block(db, member);
        std::pair<id_type, id_type> const pair{member, parent};
        if (b.added.erase(pair) > 0) {
            b.dirty = true;
        } else if (std::binary_search(b.pairs.begin(), b.pairs.end(), pair)) {
            b.dirty |= b.removed.insert(pair).second;
        }
    }

    /// Location of a node not in the changes: loose store or a parent way.
    std::optional<osmium::Location> stored_location(id_type node,
                                                    id_type except_way = 0)
    {
        if (auto const *l = loose(node)) {
            return l->location;
        }
        for (auto const parent : parents(db_t::n2w, node)) {
            if (parent == except_way) {
                continue;
            }
            if (auto const *w = way(parent)) {
                if (auto const loc = location_in(*w, node)) {
                    return loc;
                }
            }
        }
        return std::nullopt;
    }

    /**
     * Write all changed blocks into the transaction. The blocks are encoded
     * in parallel, then written in key order (so the result doesn't depend
     * on the number of threads).
     */
    void flush()
    {
        std::vector<job_t> jobs;
        auto const *dict = m_dict;
        collect(m_ways, db_t::ways, &jobs,
                [dict](std::vector<way_t> const &objects) {
                    return encode_ways(objects, true, *dict);
                });
        collect(m_nodes, db_t::nodes, &jobs,
                [dict](std::vector<node_t> const &objects) {
                    return encode_nodes(objects, *dict);
                });
        collect(m_rels, db_t::rels, &jobs,
                [dict](std::vector<relation_t> const &objects) {
                    return encode_relations(objects, *dict);
                });
        for (auto const db : {db_t::n2w, db_t::n2r, db_t::w2r, db_t::r2r}) {
            auto const shift = index_shift(db);
            for (auto const &[key, b] : m_index.at(static_cast<unsigned>(db))) {
                if (!b.dirty) {
                    continue;
                }
                if (b.pairs.size() == b.removed.size() && b.added.empty()) {
                    jobs.emplace_back(db, key);
                    continue;
                }
                auto const *entry = &b;
                jobs.emplace_back(db, key,
                                  [entry, base = key << shift, dict]() {
                                      return encode_pairs(entry->current(),
                                                          base, *dict);
                                  });
            }
        }
        run(&jobs);
    }

    std::uint64_t blocks_written() const noexcept { return m_written; }
    std::uint64_t bytes_written() const noexcept { return m_bytes; }

    /// Estimated size of the changed blocks (at least a page each).
    std::size_t dirty_bytes() const
    {
        constexpr std::size_t MIN_BLOCK = 4096;
        std::size_t bytes = 0;
        auto const add = [&](auto const &cache) {
            for (auto const &[key, entry] : cache) {
                if (entry.dirty) {
                    bytes += std::max(entry.stored, MIN_BLOCK);
                }
            }
        };
        add(m_ways);
        add(m_nodes);
        add(m_rels);
        for (auto const &cache : m_index) {
            add(cache);
        }
        return bytes;
    }

    /// Continue in a new transaction after flush() and commit.
    void restart(txn_t *txn)
    {
        m_txn = txn;
        m_ways.clear();
        m_nodes.clear();
        m_rels.clear();
        for (auto &cache : m_index) {
            cache.clear();
        }
    }

private:
    template <typename T>
    struct entry_t
    {
        std::vector<T> objects;
        std::size_t stored = 0; ///< size of the block when it was read
        bool dirty = false;
    };

    /// A block to write: encode() gives the value, no encode() means delete.
    struct job_t
    {
        job_t(db_t d, id_type k, std::function<std::string()> e = {})
        : db(d), key(k), encode(std::move(e))
        {
        }

        db_t db;
        id_type key;
        std::function<std::string()> encode;
        std::string value;
    };

    template <typename T>
    using cache_t = std::map<id_type, entry_t<T>>;

    /**
     * An index block: the stored pairs (sorted) and the changes to them,
     * kept apart because inserting into large sorted vectors is quadratic
     * on imports and copying whole blocks into sets is slow.
     */
    struct index_entry_t
    {
        pairs_t pairs;
        std::set<std::pair<id_type, id_type>> added;   ///< not in pairs
        std::set<std::pair<id_type, id_type>> removed; ///< in pairs
        std::size_t stored = 0; ///< size of the block when it was read
        bool dirty = false;

        /// The pairs with the changes applied (sorted).
        pairs_t current() const
        {
            pairs_t result;
            result.reserve(pairs.size() - removed.size() + added.size());
            auto a = added.begin();
            for (auto const &pair : pairs) {
                for (; a != added.end() && *a < pair; ++a) {
                    result.push_back(*a);
                }
                if (removed.empty() || !removed.count(pair)) {
                    result.push_back(pair);
                }
            }
            result.insert(result.end(), a, added.end());
            return result;
        }
    };

    template <typename T>
    std::vector<T> &block(cache_t<T> *cache, db_t db, id_type key)
    {
        auto const it = cache->find(key);
        if (it != cache->end()) {
            return it->second.objects;
        }
        auto &entry = (*cache)[key];
        if (auto const value = m_txn->get(db, key)) {
            entry.stored = value->size();
            if constexpr (std::is_same_v<T, way_t>) {
                entry.objects = decode_ways(*value, key, *m_dict);
            } else if constexpr (std::is_same_v<T, node_t>) {
                entry.objects = decode_nodes(*value, key, *m_dict);
            } else {
                entry.objects = decode_relations(*value, key, *m_dict);
            }
        }
        return entry.objects;
    }

    index_entry_t &index_block(db_t db, id_type member)
    {
        auto const shift = index_shift(db);
        auto const key = member >> shift;
        auto &cache = m_index.at(static_cast<unsigned>(db));
        auto const it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
        auto &entry = cache[key];
        if (auto const value = m_txn->get(db, key)) {
            entry.stored = value->size();
            entry.pairs = decode_pairs(*value, key << shift, *m_dict);
        }
        return entry;
    }


    void put(db_t db, id_type key, std::string const &value)
    {
        m_txn->put(db, key, value);
        ++m_written;
        m_bytes += value.size();
    }

    template <typename T, typename ENCODE>
    static void collect(cache_t<T> const &cache, db_t db,
                        std::vector<job_t> *jobs, ENCODE encode)
    {
        for (auto const &[key, entry] : cache) {
            if (!entry.dirty) {
                continue;
            }
            if (entry.objects.empty()) {
                jobs->emplace_back(db, key);
                continue;
            }
            auto const *objects = &entry.objects;
            jobs->emplace_back(
                db, key, [objects, encode]() { return encode(*objects); });
        }
    }

    /// Encode the jobs in batches with all threads, write them in order.
    void run(std::vector<job_t> *jobs)
    {
        constexpr std::size_t BATCH = 4096;
        for (std::size_t first = 0; first < jobs->size(); first += BATCH) {
            std::size_t const last = std::min(first + BATCH, jobs->size());
            std::atomic<std::size_t> next{first};
            std::exception_ptr error;
            std::mutex mutex;
            auto const work = [&]() {
                try {
                    for (std::size_t i = next++; i < last; i = next++) {
                        auto &job = (*jobs)[i];
                        if (job.encode) {
                            job.value = job.encode();
                        }
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> const guard{mutex};
                    error = std::current_exception();
                    next = last;
                }
            };
            unsigned const threads = static_cast<unsigned>(
                std::min<std::size_t>(m_threads, last - first));
            std::vector<std::thread> workers;
            for (unsigned t = 1; t < threads; ++t) {
                workers.emplace_back(work);
            }
            work();
            for (auto &worker : workers) {
                worker.join();
            }
            if (error) {
                std::rethrow_exception(error);
            }
            for (std::size_t i = first; i < last; ++i) {
                auto &job = (*jobs)[i];
                if (job.encode) {
                    put(job.db, job.key, job.value);
                } else {
                    m_txn->del(job.db, job.key);
                }
                std::string{}.swap(job.value);
            }
        }
    }

    txn_t *m_txn;
    tag_dict_t const *m_dict;
    unsigned m_threads;
    cache_t<way_t> m_ways;
    cache_t<node_t> m_nodes;
    cache_t<relation_t> m_rels;
    std::array<std::map<id_type, index_entry_t>, NUM_DBS> m_index;
    std::uint64_t m_written = 0;
    std::uint64_t m_bytes = 0;
}; // class updater_t

/**
 * Limit for the changes written in one transaction. LMDB is copy-on-write:
 * a transaction writes new copies of all pages it changes, and the pages it
 * frees can only be reused by later transactions (the previous snapshot stays
 * valid). So the database file grows by the size of the largest transaction
 * and never shrinks. Committing whenever the changed blocks reach this size
 * bounds that (and the memory used for them). Every object is applied
 * completely (with its index changes and locations) within one transaction,
 * so each commit leaves a consistent middle and applying the same changes
 * again gives the same result.
 */
std::atomic<std::size_t> &txn_limit()
{
    constexpr std::size_t DEFAULT_LIMIT = 64UL * 1000UL * 1000UL;
    static std::atomic<std::size_t> limit{DEFAULT_LIMIT};
    return limit;
}

/// Commit if the changes so far are larger than txn_limit().
void bound_txn(store_t const &store, std::unique_ptr<txn_t> *txn,
               updater_t *updater, std::size_t *counter)
{
    constexpr std::size_t CHECK_EVERY = 256;
    if (++*counter < CHECK_EVERY) {
        return;
    }
    *counter = 0;
    auto const bytes = updater->dirty_bytes();
    if (bytes < txn_limit()) {
        return;
    }
    log_debug("CODA middle: commit after {:.1f} MB of changes.",
              static_cast<double>(bytes) / 1e6);
    updater->flush();
    (*txn)->commit();
    *txn = std::make_unique<txn_t>(store, false);
    updater->restart(txn->get());
}

/// The last version of every object of type T in the buffer.
template <typename T>
std::vector<T const *> last_versions(osmium::memory::Buffer const &buffer)
{
    std::vector<T const *> objects;
    for (auto const &object : buffer.select<T>()) {
        objects.push_back(&object);
    }
    std::stable_sort(objects.begin(), objects.end(),
                     [](T const *a, T const *b) {
                         return std::make_pair(a->id(), a->version()) <
                                std::make_pair(b->id(), b->version());
                     });
    std::vector<T const *> result;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (i + 1 == objects.size() ||
            objects[i + 1]->id() != objects[i]->id()) {
            to_id(objects[i]->id());
            result.push_back(objects[i]);
        }
    }
    return result;
}

void sort_unique(std::vector<id_type> *ids)
{
    std::sort(ids->begin(), ids->end());
    ids->erase(std::unique(ids->begin(), ids->end()), ids->end());
}

// ---------------------------------------------------------------- replay

/**
 * Middle query for replaying relations: members come from a bucket of
 * resolved members, everything else from the real middle query.
 */
class member_query_t : public middle_query_t
{
public:
    explicit member_query_t(std::shared_ptr<middle_query_t> real)
    : m_real(std::move(real))
    {
    }

    void set_bucket(member_buckets_t::bucket_t const *bucket) noexcept
    {
        m_bucket = bucket;
    }

    osmium::Location get_node_location(osmid_t id) const override
    {
        return m_real->get_node_location(id);
    }

    std::size_t nodes_get_list(osmium::WayNodeList *nodes) const override
    {
        return m_real->nodes_get_list(nodes);
    }

    bool node_get(osmid_t id, osmium::memory::Buffer *buffer) const override
    {
        return m_real->node_get(id, buffer);
    }

    bool way_get(osmid_t id, osmium::memory::Buffer *buffer) const override
    {
        return m_real->way_get(id, buffer);
    }

    std::size_t
    rel_members_get(osmium::Relation const &rel, osmium::memory::Buffer *buffer,
                    osmium::osm_entity_bits::type types) const override
    {
        if (m_bucket) {
            return m_bucket->get(rel, buffer, types);
        }
        return m_real->rel_members_get(rel, buffer, types);
    }

    bool relation_get(osmid_t id, osmium::memory::Buffer *buffer) const override
    {
        return m_real->relation_get(id, buffer);
    }

private:
    std::shared_ptr<middle_query_t> m_real;
    member_buckets_t::bucket_t const *m_bucket = nullptr;
};

/// Outputs with their own Lua states for replaying in parallel.
class replay_workers_t
{
public:
    replay_workers_t(middle_t *mid, output_t const &output,
                     options_t const &options)
    {
        for (unsigned i = 0; i < options.bulk_threads; ++i) {
            auto copy_thread =
                std::make_shared<db_copy_thread_t>(options.connection_params);
            auto query =
                std::make_shared<member_query_t>(mid->get_query_instance());
            m_workers.push_back(
                {output.clone_worker(query, copy_thread), copy_thread, query});
        }
    }

    /**
     * Call func(output, key, value, buffer) for every block of db, small
     * ranges of block keys are handed out to the workers. (Collecting the
     * keys first would read all leaf pages in one thread, and as the leaf
     * pages hold the block tails, they are spread over the whole database.)
     */
    template <typename FUNC>
    void run(store_t const &store, db_t db, FUNC const &func)
    {
        constexpr id_type RANGE = 16;
        id_type end = 0;
        {
            txn_t const txn{store, true};
            end = txn.end_key(db);
        }
        parallel(
            static_cast<std::size_t>((end + RANGE - 1) / RANGE), 1,
            [&](worker_t &worker, txn_t const &txn,
                osmium::memory::Buffer *buffer, std::size_t i) {
                auto const first = static_cast<id_type>(i) * RANGE;
                txn.for_each_in(db, first, first + RANGE - 1,
                                [&](id_type key, std::string_view value) {
                                    func(worker.output.get(), key, value,
                                         buffer);
                                });
            },
            store);
    }

    /**
     * Call func(output, txn, buffer, bucket) for every bucket of resolved
     * relation members, with the members available to the output.
     */
    template <typename FUNC>
    void run_buckets(store_t const &store, member_buckets_t const &buckets,
                     FUNC const &func)
    {
        parallel(
            buckets.num_buckets(), 1,
            [&](worker_t &worker, txn_t const &txn,
                osmium::memory::Buffer *buffer, std::size_t b) {
                auto const bucket = buckets.load(b);
                worker.query->set_bucket(&bucket);
                func(worker.output.get(), txn, buffer, b);
                worker.query->set_bucket(nullptr);
            },
            store);
    }

    /// Flush everything the workers wrote to the database.
    void sync()
    {
        for (auto &worker : m_workers) {
            worker.output->sync();
        }
        for (auto &worker : m_workers) {
            worker.copy_thread->finish();
        }
    }

private:
    struct worker_t
    {
        std::shared_ptr<output_t> output;
        std::shared_ptr<db_copy_thread_t> copy_thread;
        std::shared_ptr<member_query_t> query;
    };

    /// Call func(worker, txn, buffer, i) for all i < n in the workers.
    template <typename FUNC>
    void parallel(std::size_t n, std::size_t batch, FUNC const &func,
                  store_t const &store)
    {
        std::atomic<std::size_t> next{0};
        std::mutex mutex;
        std::exception_ptr error;
        std::vector<std::thread> threads;
        threads.reserve(m_workers.size());
        for (auto &worker : m_workers) {
            threads.emplace_back([&, w = &worker]() {
                try {
                    txn_t const txn{store, true};
                    osmium::memory::Buffer buffer{
                        1024UL * 1024UL,
                        osmium::memory::Buffer::auto_grow::yes};
                    std::size_t i = 0;
                    while ((i = next.fetch_add(batch)) < n) {
                        auto const end = std::min(i + batch, n);
                        for (; i < end; ++i) {
                            func(*w, txn, &buffer, i);
                        }
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> const guard{mutex};
                    if (!error) {
                        error = std::current_exception();
                    }
                    next = n;
                }
            });
        }
        for (auto &thread : threads) {
            thread.join();
        }
        if (error) {
            std::rethrow_exception(error);
        }
    }

    std::vector<worker_t> m_workers;
};

} // anonymous namespace

// ---------------------------------------------------------------- middle

middle_coda_t::middle_coda_t(std::shared_ptr<thread_pool_t> thread_pool,
                             options_t const *options)
: middle_t(std::move(thread_pool)), m_options(options),
  m_handle(std::make_shared<coda_handle_t>())
{
    assert(options);
}

middle_coda_t::~middle_coda_t() noexcept = default;

void middle_coda_t::open(store_t::mode_t mode)
{
    if (!m_handle->store) {
        m_handle->store = std::make_unique<store_t>(m_options->coda_dir, mode);
    }
}

void middle_coda_t::open_for_changes()
{
    // Objects given to the middle on import (instead of building it with
    // build() and replay()) are applied like changes to a new, empty middle.
    // That's fine for small inputs.
    open(m_options->append ? store_t::mode_t::update : store_t::mode_t::create);
}

store_t const &middle_coda_t::store() const
{
    assert(m_handle->store);
    return *m_handle->store;
}

void middle_coda_t::start()
{
    assert(m_middle_state == middle_state::constructed);
#ifndef NDEBUG
    m_middle_state = middle_state::node;
#endif
    if (m_options->append) {
        open(store_t::mode_t::update);
    }
}

void middle_coda_t::stop()
{
    assert(m_middle_state == middle_state::done);
    if (m_handle->store && !m_options->append && !m_replaying) {
        m_handle->store->sync();
    }
    if (m_options->droptemp && !m_options->append) {
        m_handle->store.reset();
        std::filesystem::path const dir{m_options->coda_dir};
        std::filesystem::remove(dir / "data.mdb");
        std::filesystem::remove(dir / "lock.mdb");
        log_info("Removed CODA middle in '{}'.", m_options->coda_dir);
    }
}

void middle_coda_t::set_requirements(output_requirements const & /*unused*/)
{
    // The CODA middle always has full ways and relations, and full nodes
    // except for untagged nodes in ways (which only have their location).
}

void middle_coda_t::node(osmium::Node const &node)
{
    assert(m_middle_state == middle_state::node);
    if (!m_replaying) {
        m_changes.add_item(node);
        m_changes.commit();
    }
}

void middle_coda_t::way(osmium::Way const &way)
{
    assert(m_middle_state == middle_state::way);
    if (!m_replaying) {
        m_changes.add_item(way);
        m_changes.commit();
    }
}

void middle_coda_t::relation(osmium::Relation const &relation)
{
    assert(m_middle_state == middle_state::relation);
    if (!m_replaying) {
        m_changes.add_item(relation);
        m_changes.commit();
    }
}

void middle_coda_t::after_nodes()
{
    middle_t::after_nodes();
    if (!m_replaying) {
        open_for_changes();
        apply_nodes();
    }
}

void middle_coda_t::after_ways()
{
    middle_t::after_ways();
    if (!m_replaying) {
        open_for_changes();
        apply_ways();
    }
}

void middle_coda_t::after_relations()
{
    middle_t::after_relations();
    if (!m_replaying) {
        open_for_changes();
        apply_relations();
    }
}

void middle_coda_t::apply_nodes()
{
    util::timer_t timer;
    auto const nodes = last_versions<osmium::Node>(m_changes);

    std::vector<std::pair<db_t, id_type>> keys;
    std::vector<id_type> ids;
    for (auto const *node : nodes) {
        auto const id = static_cast<id_type>(node->id());
        keys.emplace_back(db_t::n2w, id >> N2W_SHIFT);
        keys.emplace_back(db_t::nodes, id >> NODE_SHIFT);
        keys.emplace_back(db_t::n2r, id >> X2R_SHIFT);
        ids.push_back(id);
    }
    std::vector<id_type> parent_ways;
    prefetch(store(), std::move(keys), &ids, &parent_ways);
    {
        std::vector<std::pair<db_t, id_type>> way_keys;
        way_keys.reserve(parent_ways.size());
        for (auto const way : parent_ways) {
            way_keys.emplace_back(db_t::ways, way >> WAY_SHIFT);
        }
        prefetch(store(), std::move(way_keys));
    }

    auto txn = std::make_unique<txn_t>(store(), false);
    updater_t u{txn.get(), store().dict(), m_options->num_procs};
    std::size_t since_check = 0;
    std::uint64_t moved = 0;
    for (auto const *node : nodes) {
        bound_txn(store(), &txn, &u, &since_check);
        auto const id = static_cast<id_type>(node->id());
        if (node->timestamp() > m_newest) {
            m_newest = node->timestamp();
        }
        if (node->deleted()) {
            m_deleted_nodes.insert(id);
            m_changed_locations.erase(id);
            u.erase_loose(id);
            continue;
        }
        m_deleted_nodes.erase(id);
        auto const location = node->location();
        m_changed_locations[id] = location;

        auto const ways = u.parents(db_t::n2w, id);
        if (!node->tags().empty() || ways.empty() ||
            u.has_parent(db_t::n2r, id)) {
            u.upsert_loose({id, location, get_tags(*node)});
        } else {
            u.erase_loose(id);
        }
        for (auto const parent : ways) {
            if (auto *w = u.way(parent)) {
                for (std::size_t i = 0; i < w->nodes.size(); ++i) {
                    if (w->nodes[i] == id && w->locations[i] != location) {
                        w->locations[i] = location;
                        u.way_dirty(parent);
                        ++moved;
                    }
                }
            }
        }
    }
    u.flush();
    txn->commit();
    m_changes.clear();

    log_debug("CODA middle: {} nodes ({} way node locations) applied in {}, "
              "{} blocks written ({:.1f} MB).",
              nodes.size(), moved, util::human_readable_duration(timer.stop()),
              u.blocks_written(), static_cast<double>(u.bytes_written()) / 1e6);
}

void middle_coda_t::apply_ways()
{
    util::timer_t timer;
    auto const ways = last_versions<osmium::Way>(m_changes);

    std::vector<std::pair<db_t, id_type>> keys;
    std::vector<id_type> refs;
    for (auto const *way : ways) {
        auto const id = static_cast<id_type>(way->id());
        keys.emplace_back(db_t::ways, id >> WAY_SHIFT);
        for (auto const &nr : way->nodes()) {
            auto const ref = to_id(nr.ref());
            keys.emplace_back(db_t::n2w, ref >> N2W_SHIFT);
            keys.emplace_back(db_t::nodes, ref >> NODE_SHIFT);
            keys.emplace_back(db_t::n2r, ref >> X2R_SHIFT);
            refs.push_back(ref);
        }
    }
    sort_unique(&refs);
    std::vector<id_type> parent_ways;
    prefetch(store(), std::move(keys), &refs, &parent_ways);
    {
        std::vector<std::pair<db_t, id_type>> way_keys;
        way_keys.reserve(parent_ways.size());
        for (auto const way : parent_ways) {
            way_keys.emplace_back(db_t::ways, way >> WAY_SHIFT);
        }
        prefetch(store(), std::move(way_keys));
    }

    auto txn = std::make_unique<txn_t>(store(), false);
    updater_t u{txn.get(), store().dict(), m_options->num_procs};
    std::size_t since_check = 0;
    std::uint64_t loose_in = 0;
    std::uint64_t loose_out = 0;
    for (auto const *way : ways) {
        bound_txn(store(), &txn, &u, &since_check);
        auto const id = static_cast<id_type>(way->id());
        if (way->timestamp() > m_newest) {
            m_newest = way->timestamp();
        }

        std::vector<id_type> old_nodes;
        std::unordered_map<id_type, osmium::Location> old_locations;
        if (auto const *w = u.way(id)) {
            for (std::size_t i = 0; i < w->nodes.size(); ++i) {
                old_nodes.push_back(w->nodes[i]);
                old_locations[w->nodes[i]] = w->locations[i];
            }
        }
        sort_unique(&old_nodes);

        std::vector<id_type> new_nodes;
        if (way->deleted()) {
            updater_t::erase(&u.way_block(id), id);
        } else {
            way_t w;
            w.id = id;
            for (auto const &nr : way->nodes()) {
                w.nodes.push_back(static_cast<id_type>(nr.ref()));
            }
            w.tags = get_tags(*way);
            w.locations.resize(w.nodes.size());
            for (std::size_t i = 0; i < w.nodes.size(); ++i) {
                auto const ref = w.nodes[i];
                if (auto const it = m_changed_locations.find(ref);
                    it != m_changed_locations.end()) {
                    w.locations[i] = it->second;
                } else if (m_deleted_nodes.count(ref)) {
                    w.locations[i] = osmium::Location{};
                } else if (auto const it2 = old_locations.find(ref);
                           it2 != old_locations.end()) {
                    w.locations[i] = it2->second;
                } else {
                    w.locations[i] =
                        u.stored_location(ref, id).value_or(osmium::Location{});
                }
            }
            new_nodes = w.nodes;
            sort_unique(&new_nodes);
            updater_t::upsert(&u.way_block(id), std::move(w));
        }
        u.way_dirty(id);

        std::vector<id_type> added;
        std::vector<id_type> removed;
        std::set_difference(new_nodes.begin(), new_nodes.end(),
                            old_nodes.begin(), old_nodes.end(),
                            std::back_inserter(added));
        std::set_difference(old_nodes.begin(), old_nodes.end(),
                            new_nodes.begin(), new_nodes.end(),
                            std::back_inserter(removed));
        for (auto const n : removed) {
            u.del_pair(db_t::n2w, n, id);
        }
        for (auto const n : added) {
            u.add_pair(db_t::n2w, n, id);
        }
        // The location of a node now in a way lives in the way: drop
        // untagged non-member nodes from the loose nodes.
        for (auto const n : added) {
            auto const *l = u.loose(n);
            if (l && l->tags.empty() && !u.has_parent(db_t::n2r, n)) {
                u.erase_loose(n);
                ++loose_out;
            }
        }
        // A node left without parent ways becomes a loose node.
        for (auto const n : removed) {
            if (m_deleted_nodes.count(n) || u.has_parent(db_t::n2w, n) ||
                u.loose(n)) {
                continue;
            }
            auto const it = m_changed_locations.find(n);
            auto const location =
                it != m_changed_locations.end() ? it->second : old_locations[n];
            if (location.valid()) {
                u.upsert_loose({n, location, {}});
                ++loose_in;
            }
        }
    }
    u.flush();
    txn->commit();
    m_changes.clear();

    log_debug("CODA middle: {} ways applied in {} ({} nodes became loose, {} "
              "no longer loose), {} blocks written ({:.1f} MB).",
              ways.size(), util::human_readable_duration(timer.stop()),
              loose_in, loose_out, u.blocks_written(),
              static_cast<double>(u.bytes_written()) / 1e6);
}

void middle_coda_t::apply_relations()
{
    util::timer_t timer;
    auto const rels = last_versions<osmium::Relation>(m_changes);

    std::vector<std::pair<db_t, id_type>> keys;
    for (auto const *rel : rels) {
        auto const id = static_cast<id_type>(rel->id());
        keys.emplace_back(db_t::rels, id >> REL_SHIFT);
        for (auto const &m : rel->members()) {
            auto const ref = to_id(m.ref());
            if (m.type() == osmium::item_type::node) {
                keys.emplace_back(db_t::n2r, ref >> X2R_SHIFT);
                keys.emplace_back(db_t::nodes, ref >> NODE_SHIFT);
                keys.emplace_back(db_t::n2w, ref >> N2W_SHIFT);
            } else {
                keys.emplace_back(
                    m.type() == osmium::item_type::way ? db_t::w2r : db_t::r2r,
                    ref >> X2R_SHIFT);
            }
        }
    }
    prefetch(store(), std::move(keys));

    auto txn = std::make_unique<txn_t>(store(), false);
    updater_t u{txn.get(), store().dict(), m_options->num_procs};
    std::size_t since_check = 0;
    std::uint64_t loose_in = 0;
    std::uint64_t loose_out = 0;
    std::array<db_t, 3> const index = {db_t::n2r, db_t::w2r, db_t::r2r};
    for (auto const *rel : rels) {
        bound_txn(store(), &txn, &u, &since_check);
        auto const id = static_cast<id_type>(rel->id());
        if (rel->timestamp() > m_newest) {
            m_newest = rel->timestamp();
        }

        std::set<std::pair<std::uint8_t, id_type>> old_members;
        std::set<std::pair<std::uint8_t, id_type>> new_members;
        if (auto const *r = u.rel(id)) {
            for (auto const &m : r->members) {
                old_members.emplace(m.type, m.ref);
            }
        }
        if (rel->deleted()) {
            updater_t::erase(&u.rel_block(id), id);
        } else {
            relation_t r;
            r.id = id;
            for (auto const &m : rel->members()) {
                auto const type = member_type(m.type());
                r.members.push_back(
                    {type, static_cast<id_type>(m.ref()), m.role()});
                new_members.emplace(type, static_cast<id_type>(m.ref()));
            }
            r.tags = get_tags(*rel);
            updater_t::upsert(&u.rel_block(id), std::move(r));
        }
        u.rel_dirty(id);

        for (auto const &[type, ref] : old_members) {
            if (new_members.count({type, ref})) {
                continue;
            }
            u.del_pair(index.at(type), ref, id);
            if (type == 0) {
                // no longer a member: drop from the loose nodes if its
                // location lives in ways
                auto const *l = u.loose(ref);
                if (l && l->tags.empty() && !u.has_parent(db_t::n2r, ref) &&
                    u.has_parent(db_t::n2w, ref)) {
                    u.erase_loose(ref);
                    ++loose_out;
                }
            }
        }
        for (auto const &[type, ref] : new_members) {
            if (old_members.count({type, ref})) {
                continue;
            }
            u.add_pair(index.at(type), ref, id);
            if (type == 0 && !u.loose(ref) && !m_deleted_nodes.count(ref)) {
                std::optional<osmium::Location> location;
                if (auto const it = m_changed_locations.find(ref);
                    it != m_changed_locations.end()) {
                    location = it->second;
                } else {
                    location = u.stored_location(ref);
                }
                if (location && location->valid()) {
                    u.upsert_loose({ref, *location, {}});
                    ++loose_in;
                }
            }
        }
    }
    u.flush();
    if (m_newest.valid()) {
        auto const current = txn->get_meta("current_timestamp");
        if (!current || m_newest > osmium::Timestamp{std::string{*current}}) {
            txn->put_meta("current_timestamp", m_newest.to_iso());
        }
    }
    txn->commit();
    m_changes.clear();
    m_changed_locations.clear();
    m_deleted_nodes.clear();

    auto const [file_size, free_size] = store().file_and_free_size();
    log_debug("CODA middle: {} relations applied in {} ({} member nodes "
              "became loose, {} no longer loose), {} blocks written ({:.1f} "
              "MB). Middle file {:.2f} GB, {:.2f} GB of it free.",
              rels.size(), util::human_readable_duration(timer.stop()),
              loose_in, loose_out, u.blocks_written(),
              static_cast<double>(u.bytes_written()) / 1e6,
              static_cast<double>(file_size) / 1e9,
              static_cast<double>(free_size) / 1e9);
}

void middle_coda_t::get_node_parents(idlist_t const &changed_nodes,
                                     idlist_t *parent_ways,
                                     idlist_t *parent_relations) const
{
    util::timer_t timer;
    std::vector<std::pair<db_t, id_type>> keys;
    for (auto const id : changed_nodes) {
        if (parent_ways) {
            keys.emplace_back(db_t::n2w, to_id(id) >> N2W_SHIFT);
        }
        keys.emplace_back(db_t::n2r, to_id(id) >> X2R_SHIFT);
    }
    prefetch(store(), keys);

    txn_t const txn{store(), true};
    index_blocks_t n2w{txn, db_t::n2w, store().dict()};
    index_blocks_t n2r{txn, db_t::n2r, store().dict()};
    std::vector<id_type> ways;
    std::vector<id_type> rels;
    for (auto const id : sorted_ids(changed_nodes)) {
        if (parent_ways) {
            for_parents(n2w.block_of(id), id,
                        [&](id_type parent) { ways.push_back(parent); });
        }
        for_parents(n2r.block_of(id), id,
                    [&](id_type parent) { rels.push_back(parent); });
    }
    sort_unique(&ways);
    sort_unique(&rels);
    if (parent_ways) {
        for (auto const w : ways) {
            parent_ways->push_back(static_cast<osmid_t>(w));
        }
    }
    for (auto const r : rels) {
        parent_relations->push_back(static_cast<osmid_t>(r));
    }
    parent_relations->sort_unique();

    log_debug("CODA middle: found {} parent ways and {} parent relations of "
              "{} nodes in {}.",
              ways.size(), rels.size(), changed_nodes.size(),
              util::human_readable_duration(timer.stop()));
}

void middle_coda_t::get_way_parents(idlist_t const &changed_ways,
                                    idlist_t *parent_relations) const
{
    std::vector<std::pair<db_t, id_type>> keys;
    for (auto const id : changed_ways) {
        keys.emplace_back(db_t::w2r, to_id(id) >> X2R_SHIFT);
    }
    prefetch(store(), keys);

    txn_t const txn{store(), true};
    index_blocks_t w2r{txn, db_t::w2r, store().dict()};
    for (auto const id : sorted_ids(changed_ways)) {
        for_parents(w2r.block_of(id), id, [&](id_type parent) {
            parent_relations->push_back(static_cast<osmid_t>(parent));
        });
    }
    parent_relations->sort_unique();
}

void middle_coda_t::set_txn_limit(std::size_t bytes) noexcept
{
    txn_limit() = bytes;
}

std::shared_ptr<middle_query_t> middle_coda_t::get_query_instance()
{
    return std::make_shared<middle_query_coda_t>(m_handle);
}

// ---------------------------------------------------------------- import

file_info middle_coda_t::build(osmium::io::File const &file)
{
    m_handle->store.reset();
    return coda::build(file, m_options->coda_dir,
                       std::max(m_options->num_procs, 1U));
}

file_info middle_coda_t::replay(osmdata_t *osmdata,
                                std::shared_ptr<output_t> const &output)
{
    m_replaying = true;
    open(store_t::mode_t::read);
    if (m_options->output_backend == "null") {
        // nothing to send, only building the middle
        osmdata->after_nodes();
        osmdata->after_ways();
        osmdata->after_relations();
        return info();
    }
    auto const start = clock_type::now();
    auto const &dict = store().dict();
    txn_t const txn{store(), true};

    std::atomic<std::uint64_t> count{0};
    auto log_done = [&](char const *what) {
        log_info("  {} {} done after {}.", count.load(), what,
                 util::human_readable_duration(elapsed(start)));
        count = 0;
    };

    auto const nodes = [&](output_t *out, id_type key, std::string_view value,
                           osmium::memory::Buffer *buffer) {
        for (auto const &node : decode_nodes(value, key, dict)) {
            if (!node.location.valid()) {
                continue; // like osmdata_t::node()
            }
            buffer->clear();
            build_node(buffer, node.id, node.location, node.tags);
            out->node_add(buffer->get<osmium::Node>(0));
            ++count;
        }
    };
    auto const ways = [&](output_t *out, id_type key, std::string_view value,
                          osmium::memory::Buffer *buffer) {
        for (auto const &way : decode_ways(value, key, dict)) {
            buffer->clear();
            build_way(buffer, way);
            out->way_add(&buffer->get<osmium::Way>(0));
            ++count;
        }
    };
    auto const relations = [&](output_t *out, id_type key,
                               std::string_view value,
                               osmium::memory::Buffer *buffer) {
        for (auto const &rel : decode_relations(value, key, dict)) {
            if (rel.members.size() > 32767) {
                continue; // like osmdata_t::relation()
            }
            buffer->clear();
            build_relation(buffer, rel);
            out->relation_add(buffer->get<osmium::Relation>(0));
            ++count;
        }
    };

    if (m_options->bulk_threads > 0) {
        log_info("Sending CODA middle to the output with {} threads...",
                 m_options->bulk_threads);
        {
            replay_workers_t workers{this, *output, *m_options};
            workers.run(store(), db_t::nodes, nodes);
            log_done("loose nodes");
            osmdata->after_nodes();
            workers.run(store(), db_t::ways, ways);
            log_done("ways");
            workers.sync();
        }
        osmdata->after_ways();
        {
            // Members are resolved with a sort-merge join, not random reads.
            member_buckets_t const buckets{
                store(),
                (std::filesystem::path{m_options->coda_dir} / "replay-tmp")
                    .string(),
                m_options->bulk_threads};
            log_info("  Relation members resolved after {}.",
                     util::human_readable_duration(elapsed(start)));
            replay_workers_t workers{this, *output, *m_options};
            workers.run_buckets(
                store(), buckets,
                [&](output_t *out, txn_t const &rtxn,
                    osmium::memory::Buffer *buffer, std::size_t b) {
                    auto const first = (static_cast<id_type>(b)
                                        << member_buckets_t::BUCKET_SHIFT) >>
                                       REL_SHIFT;
                    auto const last = ((static_cast<id_type>(b + 1)
                                        << member_buckets_t::BUCKET_SHIFT) >>
                                       REL_SHIFT) -
                                      1;
                    rtxn.for_each_in(db_t::rels, first, last,
                                     [&](id_type key, std::string_view value) {
                                         relations(out, key, value, buffer);
                                     });
                });
            log_done("relations");
            workers.sync();
        }
        osmdata->after_relations();
    } else {
        // Through osmdata, which gives the objects to the middle (which
        // ignores them) and the output.
        osmium::memory::Buffer buffer{1024UL * 1024UL,
                                      osmium::memory::Buffer::auto_grow::yes};
        auto const each = [&](db_t db, auto const &func) {
            txn.for_each(db, [&](id_type key, std::string_view value) {
                func(key, value);
            });
        };
        log_info("Sending CODA middle to the output...");
        each(db_t::nodes, [&](id_type key, std::string_view value) {
            for (auto const &node : decode_nodes(value, key, dict)) {
                buffer.clear();
                build_node(&buffer, node.id, node.location, node.tags);
                osmdata->node(buffer.get<osmium::Node>(0));
                ++count;
            }
        });
        log_done("loose nodes");
        osmdata->after_nodes();
        each(db_t::ways, [&](id_type key, std::string_view value) {
            for (auto const &way : decode_ways(value, key, dict)) {
                buffer.clear();
                build_way(&buffer, way);
                osmdata->way(buffer.get<osmium::Way>(0));
                ++count;
            }
        });
        log_done("ways");
        osmdata->after_ways();
        each(db_t::rels, [&](id_type key, std::string_view value) {
            for (auto const &rel : decode_relations(value, key, dict)) {
                buffer.clear();
                build_relation(&buffer, rel);
                osmdata->relation(buffer.get<osmium::Relation>(0));
                ++count;
            }
        });
        log_done("relations");
        osmdata->after_relations();
    }

    return info();
}

file_info middle_coda_t::info() const
{
    txn_t const txn{store(), true};
    // What the input file would have told us.
    file_info finfo;
    auto meta = [&](char const *key) {
        return std::string{txn.get_meta(key).value_or("")};
    };
    auto const import_ts = meta("import_timestamp");
    auto const current_ts = meta("current_timestamp");
    if (!current_ts.empty()) {
        finfo.last_timestamp = osmium::Timestamp{current_ts};
    }
    finfo.header.set("osmosis_replication_base_url",
                     meta("replication_base_url"));
    if (current_ts == import_ts) {
        // The middle is as imported, so the sequence number still fits.
        finfo.header.set("osmosis_replication_sequence_number",
                         meta("replication_sequence_number"));
        finfo.header.set("osmosis_replication_timestamp",
                         meta("replication_timestamp"));
    }
    return finfo;
}

file_info coda_import(std::vector<osmium::io::File> const &files,
                      std::shared_ptr<middle_t> const &mid,
                      std::shared_ptr<output_t> const &output,
                      osmdata_t *osmdata)
{
    auto *coda = dynamic_cast<middle_coda_t *>(mid.get());
    assert(coda);
    if (files.size() > 1) {
        throw std::runtime_error{
            "The CODA middle can only be built from a single input file."};
    }
    if (files.empty()) {
        log_info("Using existing CODA middle.");
    } else {
        coda->build(files.front());
    }
    return coda->replay(osmdata, output);
}
