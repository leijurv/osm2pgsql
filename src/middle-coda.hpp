#ifndef OSM2PGSQL_MIDDLE_CODA_HPP
#define OSM2PGSQL_MIDDLE_CODA_HPP

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
 * The CODA middle: a compact, updatable on-disk middle outside the database
 * (see coda-format.hpp for the format).
 *
 * On import the middle is built from the input file first (coda-build.hpp)
 * and then all objects are read back from it and sent to the output
 * (coda_replay()). Because the middle has everything the output needs, an
 * output can also be (re)created later from an existing middle without the
 * input file.
 *
 * On update the changes are collected and applied in one transaction per
 * object type (after all nodes, all ways, all relations), after reading
 * every block they touch with many threads in parallel.
 */

#include "coda-format.hpp"
#include "coda-store.hpp"
#include "input.hpp"
#include "middle.hpp"

#include <osmium/memory/buffer.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

class osmdata_t;
class output_t;
struct options_t;

/// The store, shared by the middle and its query instances.
struct coda_handle_t
{
    std::unique_ptr<coda::store_t> store;
};

class middle_coda_t : public middle_t
{
public:
    middle_coda_t(std::shared_ptr<thread_pool_t> thread_pool,
                  options_t const *options);

    ~middle_coda_t() noexcept override;

    void start() override;
    void stop() override;

    void node(osmium::Node const &node) override;
    void way(osmium::Way const &way) override;
    void relation(osmium::Relation const &relation) override;

    void after_nodes() override;
    void after_ways() override;
    void after_relations() override;

    void get_node_parents(idlist_t const &changed_nodes, idlist_t *parent_ways,
                          idlist_t *parent_relations) const override;

    void get_way_parents(idlist_t const &changed_ways,
                         idlist_t *parent_relations) const override;

    std::shared_ptr<middle_query_t> get_query_instance() override;

    /**
     * Commit updates whenever the changed blocks reach this many bytes
     * (default 64 MB). Only for tests.
     */
    static void set_txn_limit(std::size_t bytes) noexcept;

    void set_requirements(output_requirements const &requirements) override;

    /// Import: build the middle from the input file (removes existing data).
    file_info build(osmium::io::File const &file);

    /**
     * Import: send all objects in the middle to the output. The middle has
     * to be built already, possibly in an earlier run.
     */
    file_info replay(osmdata_t *osmdata,
                     std::shared_ptr<output_t> const &output);

private:
    void open(coda::store_t::mode_t mode);
    void open_for_changes();
    coda::store_t const &store() const;

    /// What the input file told us, as stored in the middle.
    file_info info() const;

    /// Apply the buffered changes of one object type.
    void apply_nodes();
    void apply_ways();
    void apply_relations();

    options_t const *m_options;
    std::shared_ptr<coda_handle_t> m_handle;

    /// Changes of the current object type, applied in after_*().
    osmium::memory::Buffer m_changes{1024UL * 1024UL,
                                     osmium::memory::Buffer::auto_grow::yes};

    /// Node locations and deletions of this run (needed for the ways).
    std::unordered_map<coda::id_type, osmium::Location> m_changed_locations;
    std::unordered_set<coda::id_type> m_deleted_nodes;

    osmium::Timestamp m_newest;

    /// Objects are sent from the middle to the output, ignore them here.
    bool m_replaying = false;
}; // class middle_coda_t

/// Called by main: build the middle if there is an input file, then replay.
file_info coda_import(std::vector<osmium::io::File> const &files,
                      std::shared_ptr<middle_t> const &mid,
                      std::shared_ptr<output_t> const &output,
                      osmdata_t *osmdata);

#endif // OSM2PGSQL_MIDDLE_CODA_HPP
