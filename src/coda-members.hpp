#ifndef OSM2PGSQL_CODA_MEMBERS_HPP
#define OSM2PGSQL_CODA_MEMBERS_HPP

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
 * Relation members for processing all relations of a CODA middle, resolved
 * with a sort-merge join instead of random reads: the way blocks are read in
 * order together with the way->relation index and every member way (its
 * node ids and locations) is written to a temporary bucket file for its
 * relation. The same is done for node members. Then relations are processed
 * bucket by bucket, with their members from memory.
 */

#include "coda-format.hpp"

#include <osmium/memory/buffer.hpp>
#include <osmium/osm/relation.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace coda {

class store_t;

class member_buckets_t
{
public:
    /// Relation ids per bucket.
    static constexpr unsigned BUCKET_SHIFT = 16;

    /// Write all members to bucket files in dir, using threads threads.
    member_buckets_t(store_t const &store, std::string dir, unsigned threads);

    ~member_buckets_t() noexcept;

    member_buckets_t(member_buckets_t const &) = delete;
    member_buckets_t &operator=(member_buckets_t const &) = delete;
    member_buckets_t(member_buckets_t &&) = delete;
    member_buckets_t &operator=(member_buckets_t &&) = delete;

    std::size_t num_buckets() const noexcept { return m_num_buckets; }

    /// The members of all relations in one bucket.
    class bucket_t
    {
    public:
        /**
         * Add the members of relation rel with types to buffer (like
         * middle_query_t::rel_members_get()). Node members that are not
         * available are added without location.
         */
        std::size_t get(osmium::Relation const &rel,
                        osmium::memory::Buffer *buffer,
                        osmium::osm_entity_bits::type types) const;

    private:
        friend class member_buckets_t;

        struct entry_t
        {
            id_type rel;
            std::uint8_t type;
            id_type id;
            std::size_t offset; ///< of the object in m_buffer
            friend bool operator<(entry_t const &a, entry_t const &b) noexcept
            {
                return std::tie(a.rel, a.type, a.id) <
                       std::tie(b.rel, b.type, b.id);
            }
        };

        osmium::memory::Buffer m_buffer{1024UL * 1024UL,
                                        osmium::memory::Buffer::auto_grow::yes};
        std::vector<entry_t> m_entries; // sorted
    };

    /// Read (and delete) the files of bucket b.
    bucket_t load(std::size_t b) const;

private:
    std::string m_dir;
    unsigned m_threads;
    std::size_t m_num_buckets = 0;
};

} // namespace coda

#endif // OSM2PGSQL_CODA_MEMBERS_HPP
