#ifndef OSM2PGSQL_BULK_IMPORT_HPP
#define OSM2PGSQL_BULK_IMPORT_HPP

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

#include <memory>

class middle_t;
class osmdata_t;
class output_t;
struct options_t;

/**
 * Import a single sorted OSM file without random access to node locations.
 *
 * Way node locations are resolved with a sort-merge join through bucket
 * files in options.bulk_tmpdir, and nodes and ways are processed by
 * options.bulk_threads outputs in parallel, each with its own interpreter
 * state. Relations are processed as usual through osmdata.
 */
file_info bulk_import(osmium::io::File const &file,
                      std::shared_ptr<middle_t> const &mid,
                      std::shared_ptr<output_t> const &output,
                      options_t const &options, osmdata_t *osmdata);

#endif // OSM2PGSQL_BULK_IMPORT_HPP
