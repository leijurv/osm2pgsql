Feature: A valid geometry that is projected can become invalid

    Scenario: Valid geometry must end up in output table
        Given the OSM data
            """
            n10 t2020-01-02T03:04:05Z x0 y50
            n11 t2020-01-02T03:04:05Z x50 y50
            n12 t2020-01-02T03:04:05Z x100 y50
            n13 t2020-01-02T03:04:05Z x100 y50.1
            n14 t2020-01-02T03:04:05Z x0 y50.1
            w20 t2020-01-02T03:04:06Z Tlanduse=forest Nn10,n11,n12,n13,n14,n10
            """
        And the lua style
            """
            local polygons = osm2pgsql.define_table({
                name = 'osm2pgsql_test_polygon',
                ids = { type = 'way', id_column = 'osm_id' },
                columns = {
                    { column = 'geom', type = 'polygon', projection = 4326 },
                }
            })
            function osm2pgsql.process_way(object)
                polygons:insert({
                    geom = object:as_polygon()
                })
            end
            """
        When running osm2pgsql flex
        Then table osm2pgsql_test_polygon has 1 rows

    Scenario: Invalid geometry after projection must not end up in output table
        Given the OSM data
            """
            n10 t2020-01-02T03:04:05Z x0 y50
            n11 t2020-01-02T03:04:05Z x50 y50
            n12 t2020-01-02T03:04:05Z x100 y50
            n13 t2020-01-02T03:04:05Z x100 y50.1
            n14 t2020-01-02T03:04:05Z x0 y50.1
            w20 t2020-01-02T03:04:06Z Tlanduse=forest Nn10,n11,n12,n13,n14,n10
            """
        And the lua style
            """
            local srid = 4326

            if osm2pgsql.proj_version ~= '[disabled]' then
                srid = 3031
            end

            local polygons = osm2pgsql.define_table({
                name = 'osm2pgsql_test_polygon',
                ids = { type = 'way', id_column = 'osm_id' },
                columns = {
                    { column = 'geom', type = 'polygon', projection = srid },
                }
            })

            if osm2pgsql.proj_version ~= '[disabled]' then
                function osm2pgsql.process_way(object)
                    polygons:insert({
                        geom = object:as_polygon()
                    })
                end
            end
            """
        When running osm2pgsql flex
        Then table osm2pgsql_test_polygon has 0 rows

