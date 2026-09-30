/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include <osmium/geom/mercator_projection.hpp>

#include "format.hpp"
#include "reprojection.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <mutex>
#include <vector>

namespace {

geom::point_t lonlat2merc(geom::point_t point)
{
    osmium::geom::Coordinates coords{point.x(), point.y()};

    if (coords.y > 89.99) {
        coords.y = 89.99;
    } else if (coords.y < -89.99) {
        coords.y = -89.99;
    }

    auto c = osmium::geom::lonlat_to_mercator(coords);
    return {c.x, c.y};
}

class latlon_reprojection_t : public reprojection_t
{
public:
    geom::point_t reproject(geom::point_t point) const noexcept override
    {
        return point;
    }

    geom::point_t target_to_tile(geom::point_t point) const noexcept override
    {
        return lonlat2merc(point);
    }

    int target_srs() const noexcept override { return PROJ_LATLONG; }

    char const *target_desc() const noexcept override { return "Latlong"; }
};

class merc_reprojection_t : public reprojection_t
{
public:
    geom::point_t reproject(geom::point_t coords) const noexcept override
    {
        return lonlat2merc(coords);
    }

    geom::point_t target_to_tile(geom::point_t c) const noexcept override
    {
        return c;
    }

    int target_srs() const noexcept override { return PROJ_SPHERE_MERC; }

    char const *target_desc() const noexcept override
    {
        return "Spherical Mercator";
    }
};

} // anonymous namespace

std::shared_ptr<reprojection_t> reprojection_t::create_projection(int srs)
{
    switch (srs) {
    case PROJ_LATLONG:
        return std::make_shared<latlon_reprojection_t>();
    case PROJ_SPHERE_MERC:
        return std::make_shared<merc_reprojection_t>();
    default:
        break;
    }

    if (srs <= 0) {
        throw fmt_error("Invalid projection SRID '{}'.", srs);
    }

    return make_generic_projection(srs);
}

reprojection_t const &get_projection(int srs)
{
    // In almost all cases there will be only one or two projections used, so
    // storing them in a vector and doing linear search is totally fine. This
    // is called from several threads (with --bulk-threads), so the list is
    // protected by a mutex and each thread keeps its own list of pointers.
    thread_local std::vector<reprojection_t const *> cache;
    for (auto const *p : cache) {
        if (p->target_srs() == srs) {
            return *p;
        }
    }

    static std::mutex mutex;
    static std::vector<std::shared_ptr<reprojection_t>> projections;

    std::lock_guard<std::mutex> const guard{mutex};
    auto it =
        std::find_if(projections.begin(), projections.end(),
                     [&](auto const &p) { return p->target_srs() == srs; });
    if (it == projections.end()) {
        projections.push_back(reprojection_t::create_projection(srs));
        it = std::prev(projections.end());
    }
    cache.push_back(it->get());
    return **it;
}
