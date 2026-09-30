#ifndef OSM2PGSQL_CODA_FORMAT_HPP
#define OSM2PGSQL_CODA_FORMAT_HPP

/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

/**
 * \file
 *
 * Block format of the CODA middle (compact on-disk middle).
 *
 * All objects are stored in blocks keyed by id >> shift. A block value
 * holds all objects of that id range as a few independently compressed
 * streams. Ways carry the locations of their nodes, so only "loose" nodes
 * (tagged nodes, relation members and nodes not in any way) are stored as
 * objects. Parent indexes (node->way, node->relation, way->relation,
 * relation->relation) are blocks of (member, parent) pairs keyed by the
 * member id.
 *
 * Only positive ids are supported and object attributes (version,
 * timestamp, changeset, user) are not stored.
 */

#include <osmium/osm/item_type.hpp>
#include <osmium/osm/location.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace osmium {
class OSMObject;
} // namespace osmium

struct ZSTD_CDict_s;
struct ZSTD_DDict_s;

namespace coda {

using id_type = std::uint64_t;

/// Version of the block format, stored in the meta database.
inline constexpr std::uint32_t FORMAT_VERSION = 2;

inline constexpr unsigned WAY_SHIFT = 10;  ///< way ids per way block
inline constexpr unsigned NODE_SHIFT = 12; ///< node ids per loose node block
inline constexpr unsigned REL_SHIFT = 6;   ///< relation ids per block
inline constexpr unsigned N2W_SHIFT = 14;  ///< node ids per node->way block
inline constexpr unsigned X2R_SHIFT = 16;  ///< member ids per ->rel block

// ---------------------------------------------------------------- varints

/// Zigzag coding of signed integers: 0, -1, 1, -2, ... -> 0, 1, 2, 3, ...
constexpr std::uint64_t zigzag(std::int64_t value) noexcept
{
    return (static_cast<std::uint64_t>(value) << 1U) ^
           (value < 0 ? ~std::uint64_t{0} : std::uint64_t{0});
}

constexpr std::int64_t unzigzag(std::uint64_t z) noexcept
{
    return static_cast<std::int64_t>((z >> 1U) ^ (std::uint64_t{0} - (z & 1U)));
}

class writer_t
{
public:
    void varint(std::uint64_t value)
    {
        while (value >= 0x80U) {
            m_data.push_back(static_cast<char>(value | 0x80U));
            value >>= 7U;
        }
        m_data.push_back(static_cast<char>(value));
    }

    void svarint(std::int64_t value)
    {
        varint(zigzag(value));
    }

    void string(std::string_view str)
    {
        varint(str.size());
        m_data.append(str);
    }

    void byte(std::uint8_t b) { m_data.push_back(static_cast<char>(b)); }

    std::string &data() noexcept { return m_data; }
    std::string const &data() const noexcept { return m_data; }

private:
    std::string m_data;
};

class reader_t
{
public:
    reader_t() = default;

    explicit reader_t(std::string_view data) noexcept
    : m_ptr(reinterpret_cast<std::uint8_t const *>(data.data())),
      m_end(m_ptr + data.size())
    {
    }

    bool done() const noexcept { return m_ptr >= m_end; }

    std::uint64_t varint();

    std::int64_t svarint()
    {
        return unzigzag(varint());
    }

    std::string_view string();

    std::uint8_t byte();

private:
    std::uint8_t const *m_ptr = nullptr;
    std::uint8_t const *m_end = nullptr;
};

/**
 * "Split" coding of integers: each value is stored as a one-byte symbol
 * (its bit length times four plus the two bits below the leading one), which
 * zstd's entropy coder models well, and the remaining low bits, which are
 * close to random, raw in a separate bit stream.
 */
class split_writer_t
{
public:
    void put(std::uint64_t value);

    void sput(std::int64_t value)
    {
        put(zigzag(value));
    }

    /// Flush the last partial byte of the bit stream.
    void finish();

    std::string &symbols() noexcept { return m_symbols; }
    std::string &bits() noexcept { return m_bits; }

private:
    std::string m_symbols;
    std::string m_bits;
    std::uint64_t m_acc = 0;
    unsigned m_nbits = 0;
};

class split_reader_t
{
public:
    split_reader_t(std::string_view symbols, std::string_view bits) noexcept
    : m_symbols(symbols), m_bits(bits)
    {
    }

    std::uint64_t get();

    std::int64_t sget()
    {
        return unzigzag(get());
    }

private:
    std::string_view m_symbols;
    std::string_view m_bits;
    std::size_t m_si = 0;
    std::size_t m_bi = 0;
    std::uint64_t m_acc = 0;
    unsigned m_nbits = 0;
};

// ---------------------------------------------------------------- tags

using tags_t = std::vector<std::pair<std::string, std::string>>;

tags_t get_tags(osmium::OSMObject const &object);

/// Member type as stored: 0 node, 1 way, 2 relation.
inline std::uint8_t member_type(osmium::item_type type) noexcept
{
    return type == osmium::item_type::node  ? 0
           : type == osmium::item_type::way ? 1
                                            : 2;
}

/**
 * Static tag dictionary: frequent keys and key=value pairs are replaced by
 * small ids, then the tag stream is compressed with a zstd dictionary
 * trained on planet data. The same dictionary is used for the whole world
 * and stored in the database, so a database stays readable even if a later
 * version of osm2pgsql comes with a different default dictionary.
 */
class tag_dict_t
{
public:
    /**
     * Load a dictionary.
     *
     * \param entries Lines "K\tkey" or "T\tkey\tvalue", the line number
     *                (starting at 1) is the id. Tabs, newlines and
     *                backslashes are escaped with a backslash.
     * \param zstd_dict Trained zstd dictionary (can be empty).
     */
    tag_dict_t(std::string_view entries, std::string_view zstd_dict);

    ~tag_dict_t() noexcept;

    tag_dict_t(tag_dict_t const &) = delete;
    tag_dict_t &operator=(tag_dict_t const &) = delete;
    tag_dict_t(tag_dict_t &&) = delete;
    tag_dict_t &operator=(tag_dict_t &&) = delete;

    /// The dictionary that comes with this version of osm2pgsql.
    static std::string_view default_entries();
    static std::string_view default_zstd_dict();

    void encode(writer_t *writer, tags_t const &tags) const;
    void decode(reader_t *reader, tags_t *tags) const;

    ZSTD_CDict_s const *cdict() const noexcept { return m_cdict; }
    ZSTD_DDict_s const *ddict() const noexcept { return m_ddict; }

private:
    std::unordered_map<std::string, std::uint32_t> m_key_ids;
    std::unordered_map<std::string, std::uint32_t> m_tag_ids; // key \0 value
    std::vector<std::string> m_keys; // index = id, empty if not a key
    std::vector<std::pair<std::string, std::string>> m_tags;
    ZSTD_CDict_s *m_cdict = nullptr;
    ZSTD_DDict_s *m_ddict = nullptr;
};

// ---------------------------------------------------------------- blocks

enum class codec_t : std::uint8_t
{
    raw = 0,
    zstd = 1,
    zstd_tags = 2
};

using streams_t = std::vector<std::pair<codec_t, std::string>>;

/**
 * Pack streams into a block: u8 version, varint number of streams, per
 * stream (u8 codec, varint raw length, varint stored length), payloads.
 */
std::string pack(streams_t const &streams, tag_dict_t const &dict);
std::vector<std::string> unpack(std::string_view block, tag_dict_t const &dict);

struct way_t
{
    id_type id = 0;
    std::vector<id_type> nodes;
    /// Same size as nodes (empty for blocks written without locations).
    std::vector<osmium::Location> locations;
    tags_t tags;
};

struct node_t
{
    id_type id = 0;
    osmium::Location location;
    tags_t tags;
};

struct member_t
{
    std::uint8_t type = 0; ///< 0 node, 1 way, 2 relation
    id_type ref = 0;
    std::string role;

    friend bool operator==(member_t const &a, member_t const &b) noexcept
    {
        return a.type == b.type && a.ref == b.ref && a.role == b.role;
    }
};

struct relation_t
{
    id_type id = 0;
    std::vector<member_t> members;
    tags_t tags;
};

/// (member id, parent id) pairs.
using pairs_t = std::vector<std::pair<id_type, id_type>>;

/// All ways must be from the same block and sorted by id.
std::string encode_ways(std::vector<way_t> const &ways, bool with_locations,
                        tag_dict_t const &dict);
std::vector<way_t> decode_ways(std::string_view value, id_type block,
                               tag_dict_t const &dict, bool with_tags = true);

std::string encode_nodes(std::vector<node_t> const &nodes,
                         tag_dict_t const &dict);
std::vector<node_t> decode_nodes(std::string_view value, id_type block,
                                 tag_dict_t const &dict);

std::string encode_relations(std::vector<relation_t> const &relations,
                             tag_dict_t const &dict);
std::vector<relation_t> decode_relations(std::string_view value, id_type block,
                                         tag_dict_t const &dict);

/// Pairs of one index block, base = block << shift. Pairs are sorted.
std::string encode_pairs(pairs_t const &pairs, id_type base,
                         tag_dict_t const &dict);
pairs_t decode_pairs(std::string_view value, id_type base,
                     tag_dict_t const &dict);

/// Find object with the given id in a vector sorted by id.
template <typename T>
T *find_by_id(std::vector<T> *objects, id_type id)
{
    auto const it = std::lower_bound(
        objects->begin(), objects->end(), id,
        [](T const &obj, id_type i) noexcept { return obj.id < i; });
    return (it != objects->end() && it->id == id) ? &*it : nullptr;
}

} // namespace coda

#endif // OSM2PGSQL_CODA_FORMAT_HPP
