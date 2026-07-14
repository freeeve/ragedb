/*
 * Copyright RageDB Contributors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <catch2/catch.hpp>
#include <Graph.h>
#include "../../src/gql/GqlParser.h"
#include "../../src/gql/GqlOptimizer.h"
#include "../../src/gql/GqlExecutor.h"

using namespace ragedb;
using namespace ragedb::gql;

/*
 * ISO GQL OFFSET (task 032): the page is [offset, offset + limit) of the ORDERED result. Every path that
 * prunes rows early -- the streamed top-K heap, the streamed group folds, the sort-with-limit sites and the
 * scan-limit pushdown -- has to retain offset + limit rows rather than just limit, or it throws away
 * exactly the rows the page returns. These tests pin that across the paths.
 *
 * Data: ten people named P0..P9 with rank 0..9, so an ordered projection is trivially predictable, plus a
 * Post per person (rank + 1 of them) to drive the grouped/aggregate paths.
 */
static void populate_paging_graph(Graph& graph) {
    graph.Clear();

    graph.shard.local().NodeTypeInsertPeered("Person").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "name", "string").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "rank", "integer").get();
    graph.shard.local().NodeTypeInsertPeered("Post").get();
    graph.shard.local().RelationshipTypeInsertPeered("HAS_CREATOR").get();

    for (int i = 0; i < 10; ++i) {
        std::string name = "P" + std::to_string(i);
        uint64_t p = graph.shard.local().NodeAddPeered(
            "Person", name, "{\"name\": \"" + name + "\", \"rank\": " + std::to_string(i) + "}").get();

        // Person i creates i + 1 posts, so a count grouped by person orders the same way as rank.
        for (int j = 0; j <= i; ++j) {
            uint64_t post = graph.shard.local().NodeAddPeered(
                "Post", name + "_post" + std::to_string(j), "{}").get();
            graph.shard.local().RelationshipAddPeered("HAS_CREATOR", post, p, "{}").get();
        }
    }
}

TEST_CASE("ISO GQL OFFSET pages the ordered result (task 032)", "[gql_executor_paging][task032_offset]") {
    auto graph = Graph("gql_paging_test");
    graph.Start().get();
    populate_paging_graph(graph);

    auto run = [&graph](const std::string& query_str) {
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);
        return GqlExecutor::execute(graph, std::move(query)).get();
    };

    SECTION("OFFSET skips the first rows of the ordered result") {
        std::string res = run("MATCH (p:Person) RETURN p.name AS n ORDER BY p.rank ASC OFFSET 3 LIMIT 2");
        INFO("result: " << res);
        // Ranks 0,1,2 are skipped; the page is P3, P4.
        REQUIRE(res.find("P3") != std::string::npos);
        REQUIRE(res.find("P4") != std::string::npos);
        REQUIRE(res.find("P2") == std::string::npos);
        REQUIRE(res.find("P5") == std::string::npos);
    }

    SECTION("LIMIT before OFFSET parses to the same page") {
        REQUIRE(run("MATCH (p:Person) RETURN p.name AS n ORDER BY p.rank ASC LIMIT 2 OFFSET 3") ==
                run("MATCH (p:Person) RETURN p.name AS n ORDER BY p.rank ASC OFFSET 3 LIMIT 2"));
    }

    SECTION("OFFSET without LIMIT returns the rest of the ordered result") {
        std::string res = run("MATCH (p:Person) RETURN p.name AS n ORDER BY p.rank ASC OFFSET 8");
        INFO("result: " << res);
        REQUIRE(res.find("P8") != std::string::npos);
        REQUIRE(res.find("P9") != std::string::npos);
        REQUIRE(res.find("P7") == std::string::npos);
    }

    SECTION("an OFFSET past the end returns no rows") {
        REQUIRE(run("MATCH (p:Person) RETURN p.name AS n ORDER BY p.rank ASC OFFSET 50 LIMIT 5") == "[]");
    }

    SECTION("consecutive pages tile the ordered result without gaps or repeats") {
        // The union of OFFSET 0 LIMIT 5 and OFFSET 5 LIMIT 5 must be every person, each once.
        std::string first = run("MATCH (p:Person) RETURN p.name AS n ORDER BY p.rank ASC OFFSET 0 LIMIT 5");
        std::string second = run("MATCH (p:Person) RETURN p.name AS n ORDER BY p.rank ASC OFFSET 5 LIMIT 5");
        INFO("first: " << first << "  second: " << second);
        for (int i = 0; i < 5; ++i) {
            std::string name = "P" + std::to_string(i);
            REQUIRE(first.find("\"" + name + "\"") != std::string::npos);
            REQUIRE(second.find("\"" + name + "\"") == std::string::npos);
        }
        for (int i = 5; i < 10; ++i) {
            std::string name = "P" + std::to_string(i);
            REQUIRE(second.find("\"" + name + "\"") != std::string::npos);
            REQUIRE(first.find("\"" + name + "\"") == std::string::npos);
        }
    }

    SECTION("OFFSET pages a grouped aggregate, not just a plain projection") {
        // Person i has i + 1 posts, so DESC by count puts P9 (10) first; skipping 2 lands on P7 (8).
        std::string res = run(
            "MATCH (post:Post)-[:HAS_CREATOR]->(p:Person) "
            "RETURN p.name AS n, count(post) AS c ORDER BY count(post) DESC OFFSET 2 LIMIT 1");
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": \"P7\"") != std::string::npos);
        REQUIRE(res.find("\"c\": 8") != std::string::npos);
        REQUIRE(res.find("P9") == std::string::npos);
        REQUIRE(res.find("P8") == std::string::npos);
    }

    SECTION("a paged query returns the same window the unpaged result would have yielded") {
        // The scan-limit pushdown and the top-K heap both prune early; if either kept only `limit` rows
        // instead of offset + limit, this page would come back short or wrong.
        std::string paged = run("MATCH (p:Person) RETURN p.name AS n ORDER BY p.rank ASC OFFSET 6 LIMIT 3");
        INFO("paged: " << paged);
        REQUIRE(paged.find("P6") != std::string::npos);
        REQUIRE(paged.find("P7") != std::string::npos);
        REQUIRE(paged.find("P8") != std::string::npos);
        REQUIRE(paged.find("P9") == std::string::npos);
        REQUIRE(paged.find("P5") == std::string::npos);
    }

    graph.Stop().get();
}
