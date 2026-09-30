/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "coda-format.hpp"

#include <osmium/osm/object.hpp>

#include <zstd.h>

#include <array>
#include <stdexcept>
#include <tuple>

namespace coda {

namespace {

std::runtime_error corrupt(char const *what)
{
    return std::runtime_error{std::string{"CODA middle: corrupt block ("} +
                              what + ")."};
}

unsigned bit_length(std::uint64_t value) noexcept
{
    unsigned n = 0;
    while (value) {
        ++n;
        value >>= 1U;
    }
    return n;
}

/// Compression contexts, one set per thread.
struct zstd_contexts_t
{
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    std::string buffer;

    zstd_contexts_t() = default;
    zstd_contexts_t(zstd_contexts_t const &) = delete;
    zstd_contexts_t &operator=(zstd_contexts_t const &) = delete;
    zstd_contexts_t(zstd_contexts_t &&) = delete;
    zstd_contexts_t &operator=(zstd_contexts_t &&) = delete;

    ~zstd_contexts_t() noexcept
    {
        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
    }
};

zstd_contexts_t &zstd_contexts()
{
    thread_local zstd_contexts_t contexts;
    return contexts;
}

constexpr int ZSTD_LEVEL = 3;

std::string unescape(std::string_view str)
{
    std::string result;
    result.reserve(str.size());
    for (std::size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\\' && i + 1 < str.size()) {
            char const c = str[++i];
            result.push_back(c == 't' ? '\t' : c == 'n' ? '\n' : c);
        } else {
            result.push_back(str[i]);
        }
    }
    return result;
}

unsigned slot_of(id_type id, unsigned shift) noexcept
{
    return static_cast<unsigned>(id & ((1ULL << shift) - 1U));
}

} // anonymous namespace

// ---------------------------------------------------------------- varints

std::uint64_t reader_t::varint()
{
    std::uint64_t result = 0;
    unsigned shift = 0;
    while (true) {
        if (m_ptr >= m_end || shift > 63) {
            throw corrupt("varint");
        }
        std::uint8_t const c = *m_ptr++;
        result |= static_cast<std::uint64_t>(c & 0x7fU) << shift;
        if (c < 0x80U) {
            return result;
        }
        shift += 7;
    }
}

std::string_view reader_t::string()
{
    auto const size = varint();
    if (static_cast<std::uint64_t>(m_end - m_ptr) < size) {
        throw corrupt("string");
    }
    std::string_view const result{reinterpret_cast<char const *>(m_ptr), size};
    m_ptr += size;
    return result;
}

std::uint8_t reader_t::byte()
{
    if (m_ptr >= m_end) {
        throw corrupt("byte");
    }
    return *m_ptr++;
}

void split_writer_t::put(std::uint64_t value)
{
    unsigned const b = bit_length(value);
    // bits folded into the symbol, and raw bits below them
    unsigned const e = b >= 3 ? 2 : (b == 2 ? 1 : 0);
    unsigned k = b > 1 ? b - 1 - e : 0;
    unsigned const top =
        e ? static_cast<unsigned>((value >> k) & ((1U << e) - 1U)) : 0U;
    m_symbols.push_back(static_cast<char>(b * 4 + top));

    std::uint64_t m = value & ((1ULL << k) - 1U);
    while (k > 0) {
        unsigned const t = std::min(k, 32U); // m_nbits < 8 here
        m_acc |= (m & ((1ULL << t) - 1U)) << m_nbits;
        m_nbits += t;
        m >>= t;
        k -= t;
        while (m_nbits >= 8) {
            m_bits.push_back(static_cast<char>(m_acc & 0xffU));
            m_acc >>= 8U;
            m_nbits -= 8;
        }
    }
}

void split_writer_t::finish()
{
    if (m_nbits > 0) {
        m_bits.push_back(static_cast<char>(m_acc & 0xffU));
        m_acc = 0;
        m_nbits = 0;
    }
}

std::uint64_t split_reader_t::get()
{
    if (m_si >= m_symbols.size()) {
        throw corrupt("split symbols");
    }
    unsigned const sym = static_cast<std::uint8_t>(m_symbols[m_si++]);
    unsigned const b = sym >> 2U;
    if (b <= 1) {
        return b;
    }
    unsigned const e = b >= 3 ? 2 : 1;
    unsigned const k = b - 1 - e;
    std::uint64_t m = 0;
    unsigned got = 0;
    while (got < k) {
        if (m_nbits == 0) {
            if (m_bi >= m_bits.size()) {
                throw corrupt("split bits");
            }
            m_acc = static_cast<std::uint8_t>(m_bits[m_bi++]);
            m_nbits = 8;
        }
        unsigned const t = std::min(m_nbits, k - got);
        m |= (m_acc & ((1ULL << t) - 1U)) << got;
        m_acc >>= t;
        m_nbits -= t;
        got += t;
    }
    return (1ULL << (b - 1)) | (static_cast<std::uint64_t>(sym & 3U) << k) | m;
}

// ---------------------------------------------------------------- tags

tags_t get_tags(osmium::OSMObject const &object)
{
    tags_t tags;
    tags.reserve(object.tags().size());
    for (auto const &tag : object.tags()) {
        tags.emplace_back(tag.key(), tag.value());
    }
    return tags;
}

tag_dict_t::tag_dict_t(std::string_view entries, std::string_view zstd_dict)
{
    m_keys.emplace_back();
    m_tags.emplace_back();
    std::uint32_t id = 1;
    while (!entries.empty()) {
        auto const eol = entries.find('\n');
        auto const line = entries.substr(0, eol);
        entries.remove_prefix(eol == std::string_view::npos ? entries.size()
                                                            : eol + 1);
        auto const tab = line.find('\t');
        if (line.size() < 2 || tab != 1) {
            throw std::runtime_error{"CODA middle: bad tag dictionary line."};
        }
        m_keys.emplace_back();
        m_tags.emplace_back();
        if (line[0] == 'K') {
            auto key = unescape(line.substr(2));
            m_key_ids.emplace(key, id);
            m_keys[id] = std::move(key);
        } else {
            auto const tab2 = line.find('\t', 2);
            if (tab2 == std::string_view::npos) {
                throw std::runtime_error{
                    "CODA middle: bad tag dictionary line."};
            }
            auto key = unescape(line.substr(2, tab2 - 2));
            auto value = unescape(line.substr(tab2 + 1));
            std::string kv{key};
            kv.push_back('\0');
            kv.append(value);
            m_tag_ids.emplace(std::move(kv), id);
            m_tags[id] = {std::move(key), std::move(value)};
        }
        ++id;
    }

    if (!zstd_dict.empty()) {
        m_cdict =
            ZSTD_createCDict(zstd_dict.data(), zstd_dict.size(), ZSTD_LEVEL);
        m_ddict = ZSTD_createDDict(zstd_dict.data(), zstd_dict.size());
        if (!m_cdict || !m_ddict) {
            throw std::runtime_error{"CODA middle: bad zstd dictionary."};
        }
    }
}

tag_dict_t::~tag_dict_t() noexcept
{
    ZSTD_freeCDict(m_cdict);
    ZSTD_freeDDict(m_ddict);
}

void tag_dict_t::encode(writer_t *writer, tags_t const &tags) const
{
    writer->varint(tags.size());
    std::string kv;
    for (auto const &[key, value] : tags) {
        kv.assign(key);
        kv.push_back('\0');
        kv.append(value);
        if (auto const it = m_tag_ids.find(kv); it != m_tag_ids.end()) {
            writer->varint(it->second);
        } else if (auto const it2 = m_key_ids.find(key);
                   it2 != m_key_ids.end()) {
            writer->varint(it2->second);
            writer->string(value);
        } else {
            writer->varint(0);
            writer->string(key);
            writer->string(value);
        }
    }
}

void tag_dict_t::decode(reader_t *reader, tags_t *tags) const
{
    auto const n = reader->varint();
    tags->clear();
    tags->reserve(n);
    for (std::uint64_t i = 0; i < n; ++i) {
        auto const id = reader->varint();
        if (id == 0) {
            auto const key = reader->string();
            auto const value = reader->string();
            tags->emplace_back(key, value);
        } else if (id < m_keys.size() && !m_keys[id].empty()) {
            tags->emplace_back(m_keys[id], reader->string());
        } else if (id < m_tags.size() && !m_tags[id].first.empty()) {
            tags->push_back(m_tags[id]);
        } else {
            throw corrupt("tag id");
        }
    }
}

// ---------------------------------------------------------------- blocks

std::string pack(streams_t const &streams, tag_dict_t const &dict)
{
    constexpr std::size_t MIN_COMPRESS = 32;
    auto &z = zstd_contexts();
    std::vector<std::pair<codec_t, std::string>> stored;
    stored.reserve(streams.size());
    for (auto const &[codec, data] : streams) {
        if (codec == codec_t::raw || data.size() < MIN_COMPRESS) {
            stored.emplace_back(codec_t::raw, data);
            continue;
        }
        z.buffer.resize(ZSTD_compressBound(data.size()));
        std::size_t const size =
            (codec == codec_t::zstd_tags && dict.cdict())
                ? ZSTD_compress_usingCDict(z.cctx, z.buffer.data(),
                                           z.buffer.size(), data.data(),
                                           data.size(), dict.cdict())
                : ZSTD_compressCCtx(z.cctx, z.buffer.data(), z.buffer.size(),
                                    data.data(), data.size(), ZSTD_LEVEL);
        if (ZSTD_isError(size)) {
            throw std::runtime_error{"CODA middle: zstd compression failed."};
        }
        if (size >= data.size()) {
            stored.emplace_back(codec_t::raw, data);
        } else {
            stored.emplace_back(codec, z.buffer.substr(0, size));
        }
    }

    writer_t w;
    w.byte(1);
    w.varint(stored.size());
    for (std::size_t i = 0; i < stored.size(); ++i) {
        w.byte(static_cast<std::uint8_t>(stored[i].first));
        w.varint(streams[i].second.size());
        w.varint(stored[i].second.size());
    }
    for (auto const &s : stored) {
        w.data().append(s.second);
    }
    return std::move(w.data());
}

std::vector<std::string> unpack(std::string_view block, tag_dict_t const &dict)
{
    reader_t r{block};
    if (r.byte() != 1) {
        throw corrupt("block version");
    }
    auto const n = r.varint();
    std::vector<std::tuple<std::uint8_t, std::size_t, std::size_t>> header;
    std::size_t total = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        auto const codec = r.byte();
        auto const raw_size = r.varint();
        auto const stored_size = r.varint();
        header.emplace_back(codec, raw_size, stored_size);
        total += stored_size;
    }
    if (total > block.size()) {
        throw corrupt("block size");
    }
    auto const payload = block.substr(block.size() - total);

    auto &z = zstd_contexts();
    std::vector<std::string> result;
    result.reserve(header.size());
    std::size_t offset = 0;
    for (auto const &[codec, raw_size, stored_size] : header) {
        auto const data = payload.substr(offset, stored_size);
        offset += stored_size;
        if (codec == static_cast<std::uint8_t>(codec_t::raw)) {
            result.emplace_back(data);
            continue;
        }
        std::string out(raw_size, '\0');
        std::size_t const got =
            (codec == static_cast<std::uint8_t>(codec_t::zstd_tags) &&
             dict.ddict())
                ? ZSTD_decompress_usingDDict(z.dctx, out.data(), raw_size,
                                             data.data(), data.size(),
                                             dict.ddict())
                : ZSTD_decompressDCtx(z.dctx, out.data(), raw_size, data.data(),
                                      data.size());
        if (ZSTD_isError(got) || got != raw_size) {
            throw corrupt("zstd");
        }
        result.push_back(std::move(out));
    }
    return result;
}

// Way block: presence (bitmap, or count + slot deltas for sparse blocks),
// node counts (n << 1 | closed), node id delta chain, location deltas (split
// coding: symbols, raw bits), tags. The first node of a closed way is not
// repeated at the end.
std::string encode_ways(std::vector<way_t> const &ways, bool with_locations,
                        tag_dict_t const &dict)
{
    constexpr unsigned SLOTS = 1U << WAY_SHIFT;
    std::string bitmap(SLOTS / 8, '\0');
    writer_t counts;
    writer_t ids;
    writer_t tags;
    split_writer_t locs;
    std::int64_t pid = 0;
    std::int64_t px = 0;
    std::int64_t py = 0;
    for (auto const &way : ways) {
        auto const slot = slot_of(way.id, WAY_SHIFT);
        bitmap[slot / 8] = static_cast<char>(
            static_cast<unsigned>(bitmap[slot / 8]) | (1U << (slot % 8)));
        auto const n = way.nodes.size();
        bool const closed =
            n >= 2 && way.nodes.front() == way.nodes.back() &&
            (!with_locations || way.locations.front() == way.locations.back());
        auto const ne = closed ? n - 1 : n;
        counts.varint((static_cast<std::uint64_t>(ne) << 1U) |
                      (closed ? 1U : 0U));
        for (std::size_t i = 0; i < ne; ++i) {
            auto const id = static_cast<std::int64_t>(way.nodes[i]);
            ids.svarint(id - pid);
            pid = id;
            if (with_locations) {
                auto const &loc = way.locations[i];
                locs.sput(static_cast<std::int64_t>(loc.x()) - px);
                locs.sput(static_cast<std::int64_t>(loc.y()) - py);
                px = loc.x();
                py = loc.y();
            }
        }
        dict.encode(&tags, way.tags);
    }
    locs.finish();

    std::string presence;
    if (ways.size() * 2 + 2 < bitmap.size()) {
        writer_t list;
        list.byte(1);
        list.varint(ways.size());
        unsigned prev = 0;
        for (auto const &way : ways) {
            auto const slot = slot_of(way.id, WAY_SHIFT);
            list.varint(slot - prev);
            prev = slot;
        }
        presence = std::move(list.data());
    } else {
        presence.push_back('\0');
        presence += bitmap;
    }

    return pack({{codec_t::raw, std::move(presence)},
                 {codec_t::zstd, std::move(counts.data())},
                 {codec_t::zstd, std::move(ids.data())},
                 {codec_t::zstd, std::move(locs.symbols())},
                 {codec_t::raw, std::move(locs.bits())},
                 {codec_t::zstd_tags, std::move(tags.data())}},
                dict);
}

std::vector<way_t> decode_ways(std::string_view value, id_type block,
                               tag_dict_t const &dict, bool with_tags)
{
    auto const st = unpack(value, dict);
    if (st.size() != 6 || st[0].empty()) {
        throw corrupt("way block");
    }

    std::vector<unsigned> slots;
    if (static_cast<std::uint8_t>(st[0][0]) == 1) {
        reader_t list{std::string_view{st[0]}.substr(1)};
        auto const n = list.varint();
        unsigned prev = 0;
        for (std::uint64_t i = 0; i < n; ++i) {
            prev += static_cast<unsigned>(list.varint());
            slots.push_back(prev);
        }
    } else {
        if (st[0].size() != 1 + (1U << WAY_SHIFT) / 8) {
            throw corrupt("way presence");
        }
        for (unsigned slot = 0; slot < (1U << WAY_SHIFT); ++slot) {
            if (static_cast<unsigned>(
                    static_cast<std::uint8_t>(st[0][1 + slot / 8])) &
                (1U << (slot % 8))) {
                slots.push_back(slot);
            }
        }
    }

    reader_t counts{st[1]};
    reader_t ids{st[2]};
    reader_t tags{st[5]};
    split_reader_t locs{st[3], st[4]};
    bool const has_locations = !st[3].empty();
    std::int64_t pid = 0;
    std::int64_t px = 0;
    std::int64_t py = 0;

    std::vector<way_t> result(slots.size());
    for (std::size_t w = 0; w < slots.size(); ++w) {
        auto &way = result[w];
        way.id = (block << WAY_SHIFT) | slots[w];
        auto const c = counts.varint();
        auto const ne = c >> 1U;
        bool const closed = c & 1U;
        way.nodes.resize(ne + (closed ? 1 : 0));
        if (has_locations) {
            way.locations.resize(way.nodes.size());
        }
        for (std::size_t i = 0; i < ne; ++i) {
            pid += ids.svarint();
            way.nodes[i] = static_cast<id_type>(pid);
            if (has_locations) {
                px += locs.sget();
                py += locs.sget();
                way.locations[i] = osmium::Location{static_cast<int32_t>(px),
                                                    static_cast<int32_t>(py)};
            }
        }
        if (closed) {
            way.nodes[ne] = way.nodes[0];
            if (has_locations) {
                way.locations[ne] = way.locations[0];
            }
        }
        if (with_tags) {
            dict.decode(&tags, &way.tags);
        }
    }
    return result;
}

// Loose node block: count + id offset deltas, location deltas (split
// coding), tags.
std::string encode_nodes(std::vector<node_t> const &nodes,
                         tag_dict_t const &dict)
{
    writer_t ids;
    writer_t tags;
    split_writer_t locs;
    ids.varint(nodes.size());
    id_type prev =
        nodes.empty() ? 0 : (nodes[0].id >> NODE_SHIFT) << NODE_SHIFT;
    std::int64_t px = 0;
    std::int64_t py = 0;
    for (auto const &node : nodes) {
        ids.varint(node.id - prev);
        prev = node.id;
        locs.sput(static_cast<std::int64_t>(node.location.x()) - px);
        locs.sput(static_cast<std::int64_t>(node.location.y()) - py);
        px = node.location.x();
        py = node.location.y();
        dict.encode(&tags, node.tags);
    }
    locs.finish();
    return pack({{codec_t::zstd, std::move(ids.data())},
                 {codec_t::zstd, std::move(locs.symbols())},
                 {codec_t::raw, std::move(locs.bits())},
                 {codec_t::zstd_tags, std::move(tags.data())}},
                dict);
}

std::vector<node_t> decode_nodes(std::string_view value, id_type block,
                                 tag_dict_t const &dict)
{
    auto const st = unpack(value, dict);
    if (st.size() != 4) {
        throw corrupt("node block");
    }
    reader_t ids{st[0]};
    reader_t tags{st[3]};
    split_reader_t locs{st[1], st[2]};
    std::vector<node_t> result(ids.varint());
    id_type prev = block << NODE_SHIFT;
    std::int64_t px = 0;
    std::int64_t py = 0;
    for (auto &node : result) {
        prev += ids.varint();
        node.id = prev;
        px += locs.sget();
        py += locs.sget();
        node.location = osmium::Location{static_cast<int32_t>(px),
                                         static_cast<int32_t>(py)};
        dict.decode(&tags, &node.tags);
    }
    return result;
}

// Relation block: count + id offsets, member counts, members (zigzag delta
// per member type << 2 | type), roles, tags.
std::string encode_relations(std::vector<relation_t> const &relations,
                             tag_dict_t const &dict)
{
    writer_t ids;
    writer_t counts;
    writer_t members;
    writer_t roles;
    writer_t tags;
    ids.varint(relations.size());
    id_type prev =
        relations.empty() ? 0 : (relations[0].id >> REL_SHIFT) << REL_SHIFT;
    for (auto const &rel : relations) {
        ids.varint(rel.id - prev);
        prev = rel.id;
        counts.varint(rel.members.size());
        std::array<std::int64_t, 3> last{}; // per member type
        for (auto const &m : rel.members) {
            auto const ref = static_cast<std::int64_t>(m.ref);
            auto &prev_ref = last.at(m.type);
            members.varint((zigzag(ref - prev_ref) << 2U) | m.type);
            prev_ref = ref;
            roles.string(m.role);
        }
        dict.encode(&tags, rel.tags);
    }
    return pack({{codec_t::zstd, std::move(ids.data())},
                 {codec_t::zstd, std::move(counts.data())},
                 {codec_t::zstd, std::move(members.data())},
                 {codec_t::zstd, std::move(roles.data())},
                 {codec_t::zstd_tags, std::move(tags.data())}},
                dict);
}

std::vector<relation_t> decode_relations(std::string_view value, id_type block,
                                         tag_dict_t const &dict)
{
    auto const st = unpack(value, dict);
    if (st.size() != 5) {
        throw corrupt("relation block");
    }
    reader_t ids{st[0]};
    reader_t counts{st[1]};
    reader_t members{st[2]};
    reader_t roles{st[3]};
    reader_t tags{st[4]};
    std::vector<relation_t> result(ids.varint());
    id_type prev = block << REL_SHIFT;
    for (auto &rel : result) {
        prev += ids.varint();
        rel.id = prev;
        rel.members.resize(counts.varint());
        std::array<std::int64_t, 3> last{}; // per member type
        for (auto &m : rel.members) {
            auto const x = members.varint();
            m.type = static_cast<std::uint8_t>(x & 3U);
            if (m.type > 2) {
                throw corrupt("member type");
            }
            auto &prev_ref = last.at(m.type);
            prev_ref += unzigzag(x >> 2U);
            m.ref = static_cast<id_type>(prev_ref);
            m.role = std::string{roles.string()};
        }
        dict.decode(&tags, &rel.tags);
    }
    return result;
}

// Parent index block: runs of consecutive members with the same parent,
// grouped by parent: parent deltas, number of runs per parent - 1, run start
// (zigzag delta against the end of the previous run of the same parent, the
// first against the block base), run length - 1.
std::string encode_pairs(pairs_t const &pairs, id_type base,
                         tag_dict_t const &dict)
{
    pairs_t by_parent;
    by_parent.reserve(pairs.size());
    for (auto const &[member, parent] : pairs) {
        by_parent.emplace_back(parent, member);
    }
    std::sort(by_parent.begin(), by_parent.end());

    writer_t parents;
    writer_t runs;
    writer_t starts;
    writer_t lengths;
    id_type prev_parent = 0;
    std::size_t i = 0;
    std::vector<std::pair<id_type, id_type>> parent_runs;
    while (i < by_parent.size()) {
        auto const parent = by_parent[i].first;
        parents.varint(parent - prev_parent);
        prev_parent = parent;
        parent_runs.clear();
        while (i < by_parent.size() && by_parent[i].first == parent) {
            auto const start = by_parent[i].second;
            id_type len = 1;
            std::size_t j = i + 1;
            while (j < by_parent.size() && by_parent[j].first == parent &&
                   by_parent[j].second == start + len) {
                ++len;
                ++j;
            }
            parent_runs.emplace_back(start, len);
            i = j;
        }
        runs.varint(parent_runs.size() - 1);
        auto prev_end = static_cast<std::int64_t>(base);
        for (auto const &[start, len] : parent_runs) {
            starts.svarint(static_cast<std::int64_t>(start) - prev_end);
            prev_end = static_cast<std::int64_t>(start + len);
            lengths.varint(len - 1);
        }
    }
    return pack({{codec_t::zstd, std::move(parents.data())},
                 {codec_t::zstd, std::move(runs.data())},
                 {codec_t::zstd, std::move(starts.data())},
                 {codec_t::zstd, std::move(lengths.data())}},
                dict);
}

pairs_t decode_pairs(std::string_view value, id_type base,
                     tag_dict_t const &dict)
{
    auto const st = unpack(value, dict);
    if (st.size() != 4) {
        throw corrupt("index block");
    }
    reader_t parents{st[0]};
    reader_t runs{st[1]};
    reader_t starts{st[2]};
    reader_t lengths{st[3]};
    pairs_t result;
    id_type parent = 0;
    while (!parents.done()) {
        parent += parents.varint();
        auto const num_runs = runs.varint() + 1;
        auto prev_end = static_cast<std::int64_t>(base);
        for (std::uint64_t r = 0; r < num_runs; ++r) {
            auto const start =
                static_cast<id_type>(prev_end + starts.svarint());
            auto const len = lengths.varint() + 1;
            for (id_type k = 0; k < len; ++k) {
                result.emplace_back(start + k, parent);
            }
            prev_end = static_cast<std::int64_t>(start + len);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace coda
