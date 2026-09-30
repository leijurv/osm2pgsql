#ifndef OSM2PGSQL_CODA_BUILD_HPP
#define OSM2PGSQL_CODA_BUILD_HPP

/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "input.hpp"

#include <osmium/io/file.hpp>

#include <string>

namespace coda {

/**
 * Build a CODA middle in directory dir from a sorted OSM file, without
 * random access to anything: the way node locations are resolved with a
 * sort-merge join through temporary bucket files (in dir). The file is read
 * twice, first its ways and relations, then its nodes (for PBF files only
 * these sections are read).
 *
 * \param file    Input file (sorted, no duplicates, positive ids only).
 * \param dir     Directory for the middle. Existing data is removed.
 * \param threads Number of threads for the final phase.
 */
file_info build(osmium::io::File const &file, std::string const &dir,
                unsigned threads);

} // namespace coda

#endif // OSM2PGSQL_CODA_BUILD_HPP
