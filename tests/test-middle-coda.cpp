/**
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file is part of osm2pgsql (https://osm2pgsql.org/).
 *
 * Copyright (C) 2006-2026 by the osm2pgsql developer community.
 * For a full list of authors see the git log.
 */

#include <catch.hpp>

#include "coda-build.hpp"
#include "coda-format.hpp"
#include "coda-store.hpp"
#include "idlist.hpp"
#include "middle-coda.hpp"
#include "options.hpp"

#include "common-cleanup.hpp"

#include <osmium/io/file.hpp>
#include <osmium/opl.hpp>

#include <fstream>
#include <map>
#include <string>

namespace coda {

namespace {

tag_dict_t const &dict()
{
    static tag_dict_t const d{tag_dict_t::default_entries(),
                              tag_dict_t::default_zstd_dict()};
    return d;
}

void write_file(std::string const &name, std::string const &data)
{
    std::ofstream out{name};
    out << data;
}

/// All blocks of all databases of a middle.
std::map<std::pair<unsigned, id_type>, std::string>
all_blocks(std::string const &dir)
{
    std::map<std::pair<unsigned, id_type>, std::string> blocks;
    store_t const store{dir, store_t::mode_t::read};
    txn_t const txn{store, true};
    for (unsigned i = 0; i < NUM_DBS; ++i) {
        auto const db = static_cast<db_t>(i);
        if (db == db_t::meta) {
            continue;
        }
        txn.for_each(db, [&](id_type key, std::string_view value) {
            blocks.emplace(std::make_pair(i, key), std::string{value});
        });
    }
    return blocks;
}

options_t coda_options(std::string const &dir, bool append)
{
    options_t options;
    options.slim = true;
    options.append = append;
    options.coda_dir = dir;
    return options;
}

// The data before and after the changes below.
char const *const STATE1 = R"(n1 v1 x1.0 y1.0
n2 v1 Tamenity=bench x1.1 y1.0
n3 v1 x1.2 y1.0
n4 v1 x2.0 y2.0
n5 v1 x3.0 y3.0
n6 v1 x3.1 y3.0
n7 v1 x3.1 y3.1
w1 v1 Thighway=residential,name=Main%20%Street Nn1,n2,n3
w2 v1 Tbuilding=yes Nn5,n6,n7,n5
r1 v1 Ttype=multipolygon,landuse=grass Mw2@outer,n5@label
r2 v1 Ttype=route Mr1@,w1@
)";

char const *const CHANGES = R"(n3 v2 x1.25 y1.05
n4 v2 dD
n8 v1 x4.0 y4.0
w1 v2 Thighway=residential Nn1,n2
w3 v1 Tsomething=unusual Nn8,n1
r1 v2 Ttype=multipolygon,landuse=grass Mw2@outer
r3 v1 Tnote=node Mn3@
)";

char const *const STATE2 = R"(n1 v1 x1.0 y1.0
n2 v1 Tamenity=bench x1.1 y1.0
n3 v2 x1.25 y1.05
n5 v1 x3.0 y3.0
n6 v1 x3.1 y3.0
n7 v1 x3.1 y3.1
n8 v1 x4.0 y4.0
w1 v2 Thighway=residential Nn1,n2
w2 v1 Tbuilding=yes Nn5,n6,n7,n5
w3 v1 Tsomething=unusual Nn8,n1
r1 v2 Ttype=multipolygon,landuse=grass Mw2@outer
r2 v1 Ttype=route Mr1@,w1@
r3 v1 Tnote=node Mn3@
)";

/// Give the objects in OPL data to the middle, type by type.
void apply(middle_t *mid, char const *data)
{
    osmium::memory::Buffer buffer{4096, osmium::memory::Buffer::auto_grow::yes};
    std::string const opl{data};
    std::size_t pos = 0;
    std::vector<std::string> lines;
    while (pos < opl.size()) {
        auto const eol = opl.find('\n', pos);
        lines.push_back(opl.substr(pos, eol - pos));
        pos = eol + 1;
    }
    for (char const type : {'n', 'w', 'r'}) {
        for (auto const &line : lines) {
            if (line[0] != type) {
                continue;
            }
            osmium::opl_parse(line.c_str(), buffer);
        }
    }
    for (auto const &node : buffer.select<osmium::Node>()) {
        mid->node(node);
    }
    mid->after_nodes();
    for (auto const &way : buffer.select<osmium::Way>()) {
        mid->way(way);
    }
    mid->after_ways();
    for (auto const &rel : buffer.select<osmium::Relation>()) {
        mid->relation(rel);
    }
    mid->after_relations();
}

} // anonymous namespace

TEST_CASE("CODA split coding round trip")
{
    std::vector<std::int64_t> const values = {0,
                                              1,
                                              -1,
                                              2,
                                              -2,
                                              3,
                                              7,
                                              -8,
                                              100,
                                              -1000,
                                              123456789,
                                              -987654321,
                                              INT64_C(1) << 40,
                                              -(INT64_C(1) << 50)};
    split_writer_t w;
    for (auto const v : values) {
        w.sput(v);
    }
    w.finish();
    split_reader_t r{w.symbols(), w.bits()};
    for (auto const v : values) {
        CHECK(r.sget() == v);
    }
}

TEST_CASE("CODA way block round trip")
{
    std::vector<way_t> ways;
    for (id_type const id : {1025U, 1030U, 2047U}) {
        way_t w;
        w.id = id;
        w.nodes = {id * 10, id * 10 + 1, id * 10 + 5, id * 10};
        for (auto const n : w.nodes) {
            w.locations.emplace_back(static_cast<int32_t>(n * 7),
                                     -static_cast<int32_t>(n * 3));
        }
        w.tags = {{"highway", "residential"},
                  {"name", "Rare Name 123"},
                  {"some-unusual-key", "x"}};
        ways.push_back(w);
    }
    ways[1].nodes.push_back(99); // not closed any more
    ways[1].locations.emplace_back(5, 5);

    for (bool const with_locations : {true, false}) {
        auto const block = encode_ways(ways, with_locations, dict());
        auto const back = decode_ways(block, 1, dict());
        REQUIRE(back.size() == ways.size());
        for (std::size_t i = 0; i < ways.size(); ++i) {
            CHECK(back[i].id == ways[i].id);
            CHECK(back[i].nodes == ways[i].nodes);
            CHECK(back[i].tags == ways[i].tags);
            if (with_locations) {
                CHECK(back[i].locations == ways[i].locations);
            } else {
                CHECK(back[i].locations.empty());
            }
        }
    }
}

TEST_CASE("CODA index block round trip")
{
    pairs_t pairs = {{16384, 7}, {16385, 7}, {16386, 7}, {16390, 7},
                     {16384, 9}, {20000, 3}, {32767, 1}};
    std::sort(pairs.begin(), pairs.end());
    auto const block = encode_pairs(pairs, 16384, dict());
    CHECK(decode_pairs(block, 16384, dict()) == pairs);
}

TEST_CASE("CODA relation and node blocks round trip")
{
    std::vector<relation_t> rels(2);
    rels[0].id = 64;
    rels[0].members = {
        {1, 5, "outer"}, {0, 3, ""}, {2, 100, "sub"}, {1, 2, "inner"}};
    rels[0].tags = {{"type", "multipolygon"}};
    rels[1].id = 127;
    auto const rb = decode_relations(encode_relations(rels, dict()), 1, dict());
    REQUIRE(rb.size() == 2);
    CHECK(rb[0].id == 64);
    CHECK(rb[0].members == rels[0].members);
    CHECK(rb[0].tags == rels[0].tags);
    CHECK(rb[1].id == 127);
    CHECK(rb[1].members.empty());

    std::vector<node_t> nodes = {{4096, osmium::Location{1, 2}, {}},
                                 {4100, osmium::Location{-5, 7}, {{"a", "b"}}}};
    auto const nb = decode_nodes(encode_nodes(nodes, dict()), 1, dict());
    REQUIRE(nb.size() == 2);
    CHECK(nb[1].id == 4100);
    CHECK(nb[1].location == nodes[1].location);
    CHECK(nb[1].tags == nodes[1].tags);
}

TEST_CASE("CODA middle: build, update and query")
{
    testing::cleanup::dir_t const cleanup_a{"test-coda-a"};
    testing::cleanup::dir_t const cleanup_b{"test-coda-b"};
    testing::cleanup::dir_t const cleanup_c{"test-coda-c"};
    testing::cleanup::file_t const cleanup1{"test-coda-1.opl"};
    testing::cleanup::file_t const cleanup2{"test-coda-2.opl"};
    write_file("test-coda-1.opl", STATE1);
    write_file("test-coda-2.opl", STATE2);

    build(osmium::io::File{"test-coda-1.opl"}, "test-coda-a", 2);
    build(osmium::io::File{"test-coda-2.opl"}, "test-coda-b", 2);

    // apply the changes to the first middle
    auto const options = coda_options("test-coda-a", true);
    auto thread_pool = std::make_shared<thread_pool_t>(1U);
    {
        auto mid = std::make_shared<middle_coda_t>(thread_pool, &options);
        mid->start();
        auto const midq = mid->get_query_instance();

        // parents are computed from the data before the changes
        idlist_t const nodes{1, 3, 5};
        idlist_t ways;
        idlist_t rels;
        mid->get_node_parents(nodes, &ways, &rels);
        CHECK(ways == idlist_t{1, 2});
        CHECK(rels == idlist_t{1});
        idlist_t const changed_ways{1, 2};
        idlist_t way_parents;
        mid->get_way_parents(changed_ways, &way_parents);
        CHECK(way_parents == idlist_t{1, 2});

        apply(mid.get(), CHANGES);

        osmium::memory::Buffer buffer{1024,
                                      osmium::memory::Buffer::auto_grow::yes};

        // way with its node locations
        REQUIRE(midq->way_get(3, &buffer));
        auto &way = buffer.get<osmium::Way>(0);
        REQUIRE(way.nodes().size() == 2);
        CHECK(way.nodes()[0].location() == osmium::Location{4.0, 4.0});
        CHECK(way.nodes()[1].location() == osmium::Location{1.0, 1.0});
        CHECK(midq->nodes_get_list(&way.nodes()) == 2);
        buffer.clear();

        CHECK_FALSE(midq->way_get(4, &buffer));

        // node locations: moved (now loose), in a way only, deleted
        CHECK(midq->get_node_location(3) == osmium::Location{1.25, 1.05});
        CHECK(midq->get_node_location(6) == osmium::Location{3.1, 3.0});
        CHECK(midq->get_node_location(8) == osmium::Location{4.0, 4.0});
        CHECK_FALSE(midq->get_node_location(4).valid());

        // untagged node in a way comes back without tags
        REQUIRE(midq->node_get(6, &buffer));
        CHECK(buffer.get<osmium::Node>(0).tags().empty());
        buffer.clear();
        REQUIRE(midq->node_get(2, &buffer));
        CHECK(buffer.get<osmium::Node>(0).tags()["amenity"] ==
              std::string{"bench"});
        buffer.clear();

        // relation members with locations
        REQUIRE(midq->relation_get(2, &buffer));
        auto const &rel = buffer.get<osmium::Relation>(0);
        osmium::memory::Buffer members{1024,
                                       osmium::memory::Buffer::auto_grow::yes};
        CHECK(midq->rel_members_get(rel, &members,
                                    osmium::osm_entity_bits::way) == 1);
        auto const &member = members.get<osmium::Way>(0);
        CHECK(member.id() == 1);
        CHECK(member.nodes().size() == 2);

        // parents after the changes
        idlist_t ways2;
        idlist_t rels2;
        mid->get_node_parents(idlist_t{1, 3}, &ways2, &rels2);
        CHECK(ways2 == idlist_t{1, 3});
        CHECK(rels2 == idlist_t{3});
    }

    // updated middle is the same as one built from the new data
    CHECK(all_blocks("test-coda-a") == all_blocks("test-coda-b"));

    // objects given to the middle on import are like changes to an empty one
    {
        auto const create_options = coda_options("test-coda-c", false);
        auto mid =
            std::make_shared<middle_coda_t>(thread_pool, &create_options);
        mid->start();
        apply(mid.get(), STATE2);
        mid->stop();
    }
    CHECK(all_blocks("test-coda-c") == all_blocks("test-coda-b"));
}

} // namespace coda
