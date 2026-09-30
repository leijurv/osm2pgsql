/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "coda-members.hpp"

#include "coda-store.hpp"
#include "coda-temp.hpp"
#include "logging.hpp"

#include <osmium/builder/osm_object_builder.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace coda {

namespace {

constexpr std::size_t WRITE_BUFFER = 256UL * 1024UL;

std::string kind_name(unsigned thread) { return fmt::format("m{}", thread); }

/// Per thread: one temporary stream per relation bucket.
class member_writer_t
{
public:
    member_writer_t(std::string dir, unsigned thread)
    : m_dir(std::move(dir)), m_kind(kind_name(thread))
    {
    }

    writer_t &stream(id_type rel)
    {
        auto const b = rel >> member_buckets_t::BUCKET_SHIFT;
        if (m_streams.size() <= b) {
            m_streams.resize(b + 1);
        }
        if (!m_streams[b]) {
            m_streams[b] = std::make_unique<frame_writer_t>(
                temp_path(m_dir, m_kind.c_str(), b), WRITE_BUFFER);
        }
        m_last = m_streams[b].get();
        return m_streams[b]->writer();
    }

    /// Call after each record.
    void done() { m_last->maybe_flush(); }

    std::uint64_t close()
    {
        std::uint64_t bytes = 0;
        for (auto &s : m_streams) {
            if (s) {
                s->flush();
                bytes += s->stored();
            }
        }
        return bytes;
    }

private:
    std::string m_dir;
    std::string m_kind;
    std::vector<std::unique_ptr<frame_writer_t>> m_streams;
    frame_writer_t *m_last = nullptr;
};

/**
 * For all blocks of an index (way->rel or node->rel), in parallel: find the
 * members in their object blocks and call emit(writer, rel, object).
 */
template <typename T, typename DECODE, typename EMIT>
std::uint64_t scatter(store_t const &store, db_t index, db_t objects,
                      unsigned shift, std::string const &dir, unsigned threads,
                      DECODE const &decode, EMIT const &emit)
{
    std::vector<id_type> keys;
    {
        txn_t const txn{store, true};
        txn.for_each(index, [&](id_type key, std::string_view /*value*/) {
            keys.push_back(key);
        });
    }

    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> bytes{0};
    std::mutex mutex;
    std::exception_ptr error;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t]() {
            try {
                member_writer_t writer{dir, t};
                txn_t const txn{store, true};
                for (std::size_t i = 0; (i = next++) < keys.size();) {
                    auto const key = keys[i];
                    auto const value = txn.get(index, key);
                    if (!value) {
                        continue;
                    }
                    // pairs are sorted by member, so each object block is
                    // decoded once
                    auto const pairs = decode_pairs(
                        *value, key << index_shift(index), store.dict());
                    id_type block = ~static_cast<id_type>(0);
                    std::vector<T> objs;
                    for (auto const &[member, rel] : pairs) {
                        if ((member >> shift) != block) {
                            block = member >> shift;
                            auto const v = txn.get(objects, block);
                            objs = v ? decode(*v, block) : std::vector<T>{};
                        }
                        if (auto const *obj = find_by_id(&objs, member)) {
                            emit(&writer.stream(rel), rel, *obj);
                            writer.done();
                        }
                    }
                }
                bytes += writer.close();
            } catch (...) {
                std::lock_guard<std::mutex> const guard{mutex};
                if (!error) {
                    error = std::current_exception();
                }
                next = keys.size();
            }
        });
    }
    for (auto &w : workers) {
        w.join();
    }
    if (error) {
        std::rethrow_exception(error);
    }
    return bytes;
}

} // anonymous namespace

member_buckets_t::member_buckets_t(store_t const &store, std::string dir,
                                   unsigned threads)
: m_dir(std::move(dir)), m_threads(std::max(threads, 1U))
{
    std::filesystem::remove_all(m_dir);
    std::filesystem::create_directories(m_dir);

    {
        txn_t const txn{store, true};
        id_type last = 0;
        txn.for_each(db_t::rels,
                     [&](id_type key, std::string_view /*v*/) { last = key; });
        m_num_buckets = ((last << REL_SHIFT) >> BUCKET_SHIFT) + 1;
    }

    auto const &dict = store.dict();
    auto const way_bytes = scatter<way_t>(
        store, db_t::w2r, db_t::ways, WAY_SHIFT, m_dir, m_threads,
        [&](std::string_view v, id_type block) {
            return decode_ways(v, block, dict, false);
        },
        [](writer_t *w, id_type rel, way_t const &way) {
            w->varint(rel);
            w->byte(1);
            w->varint(way.id);
            w->varint(way.nodes.size());
            std::int64_t id = 0;
            std::int64_t x = 0;
            std::int64_t y = 0;
            for (std::size_t i = 0; i < way.nodes.size(); ++i) {
                w->svarint(static_cast<std::int64_t>(way.nodes[i]) - id);
                w->svarint(way.locations[i].x() - x);
                w->svarint(way.locations[i].y() - y);
                id = static_cast<std::int64_t>(way.nodes[i]);
                x = way.locations[i].x();
                y = way.locations[i].y();
            }
        });
    auto const node_bytes = scatter<node_t>(
        store, db_t::n2r, db_t::nodes, NODE_SHIFT, m_dir, m_threads,
        [&](std::string_view v, id_type block) {
            return decode_nodes(v, block, dict);
        },
        [](writer_t *w, id_type rel, node_t const &node) {
            w->varint(rel);
            w->byte(0);
            w->varint(node.id);
            w->svarint(node.location.x());
            w->svarint(node.location.y());
        });
    log_info("  Relation members: {:.1f} GB of ways and {:.1f} GB of nodes in "
             "temporary files.",
             static_cast<double>(way_bytes) / 1e9,
             static_cast<double>(node_bytes) / 1e9);
}

member_buckets_t::~member_buckets_t() noexcept
{
    std::error_code ec;
    std::filesystem::remove_all(m_dir, ec);
}

member_buckets_t::bucket_t member_buckets_t::load(std::size_t b) const
{
    bucket_t bucket;
    for (unsigned t = 0; t < m_threads; ++t) {
        auto const data =
            take_frames(temp_path(m_dir, kind_name(t).c_str(), b));
        reader_t r{data};
        while (!r.done()) {
            bucket_t::entry_t entry{};
            entry.rel = r.varint();
            entry.type = r.byte();
            entry.id = r.varint();
            entry.offset = bucket.m_buffer.committed();
            if (entry.type == 1) {
                osmium::builder::WayBuilder builder{bucket.m_buffer};
                builder.set_id(static_cast<osmium::object_id_type>(entry.id));
                osmium::builder::WayNodeListBuilder wnl{bucket.m_buffer,
                                                        &builder};
                auto const n = r.varint();
                std::int64_t id = 0;
                std::int64_t x = 0;
                std::int64_t y = 0;
                for (std::uint64_t i = 0; i < n; ++i) {
                    id += r.svarint();
                    x += r.svarint();
                    y += r.svarint();
                    wnl.add_node_ref(osmium::NodeRef{
                        id, osmium::Location{static_cast<int32_t>(x),
                                             static_cast<int32_t>(y)}});
                }
            } else {
                osmium::builder::NodeBuilder builder{bucket.m_buffer};
                builder.set_id(static_cast<osmium::object_id_type>(entry.id));
                auto const x = static_cast<int32_t>(r.svarint());
                auto const y = static_cast<int32_t>(r.svarint());
                builder.set_location(osmium::Location{x, y});
            }
            bucket.m_buffer.commit();
            bucket.m_entries.push_back(entry);
        }
    }
    std::sort(bucket.m_entries.begin(), bucket.m_entries.end());
    return bucket;
}

std::size_t
member_buckets_t::bucket_t::get(osmium::Relation const &rel,
                                osmium::memory::Buffer *buffer,
                                osmium::osm_entity_bits::type types) const
{
    std::size_t found = 0;
    auto const rel_id = static_cast<id_type>(rel.id());
    for (auto const &member : rel.members()) {
        if (member.ref() <= 0 ||
            !(types & osmium::osm_entity_bits::from_item_type(member.type()))) {
            continue;
        }
        auto const type = member_type(member.type());
        if (type == 2) {
            continue;
        }
        entry_t const key{rel_id, type, static_cast<id_type>(member.ref()), 0};
        auto const it =
            std::lower_bound(m_entries.begin(), m_entries.end(), key);
        if (it != m_entries.end() && it->rel == key.rel &&
            it->type == key.type && it->id == key.id) {
            buffer->add_item(m_buffer.get<osmium::memory::Item>(it->offset));
            buffer->commit();
            ++found;
        } else if (type == 0) {
            // like the pgsql middle, missing nodes are there without location
            {
                osmium::builder::NodeBuilder builder{*buffer};
                builder.set_id(member.ref());
            }
            buffer->commit();
            ++found;
        }
    }
    return found;
}

} // namespace coda
