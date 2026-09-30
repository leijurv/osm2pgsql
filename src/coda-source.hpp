#ifndef OSM2PGSQL_CODA_SOURCE_HPP
#define OSM2PGSQL_CODA_SOURCE_HPP

/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include <cstdint>
#include <memory>
#include <string>

namespace coda {

/**
 * Random access to the bytes of an input file: a local file, or a remote
 * one over HTTP(S) with Range requests (if osm2pgsql was built with
 * libcurl). Building the CODA middle only needs some sections of a PBF file,
 * so a planet file can be read straight from a server, without storing it.
 */
class byte_source_t
{
public:
    /// Returns nullptr if the name can't be opened this way.
    static std::unique_ptr<byte_source_t> open(std::string const &name);

    byte_source_t() = default;
    virtual ~byte_source_t() = default;

    byte_source_t(byte_source_t const &) = delete;
    byte_source_t &operator=(byte_source_t const &) = delete;
    byte_source_t(byte_source_t &&) = delete;
    byte_source_t &operator=(byte_source_t &&) = delete;

    virtual std::uint64_t size() const noexcept = 0;

    /// Read bytes [offset, offset + length), clipped to the size. Thread safe.
    virtual std::string read(std::uint64_t offset,
                             std::size_t length) const = 0;

    /// How many reads are worth doing in parallel.
    virtual unsigned parallelism() const noexcept = 0;
};

} // namespace coda

#endif // OSM2PGSQL_CODA_SOURCE_HPP
