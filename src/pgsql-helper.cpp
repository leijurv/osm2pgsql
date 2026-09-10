/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include "pgsql-helper.hpp"

#include "format.hpp"
#include "pgsql.hpp"
#include "pgsql-capabilities.hpp"

#include <cassert>

idlist_t get_ids_from_result(pg_result_t const &result)
{
    assert(result.num_tuples() >= 0);

    idlist_t ids;
    ids.reserve(static_cast<std::size_t>(result.num_tuples()));

    for (int i = 0; i < result.num_tuples(); ++i) {
        ids.push_back(osmium::string_to_object_id(result.get_value(i, 0)));
    }

    return ids;
}

void analyze_table(pg_conn_t const &db_connection, std::string const &schema,
                   std::string const &name)
{
    auto const qual_name = qualified_name(schema, name);
    db_connection.exec("ANALYZE {}", qual_name);
}

void drop_table_if_exists(pg_conn_t const &db_connection,
                          std::string const &schema, std::string const &name)
{
    db_connection.exec("DROP TABLE IF EXISTS {} CASCADE",
                       qualified_name(schema, name));
}
