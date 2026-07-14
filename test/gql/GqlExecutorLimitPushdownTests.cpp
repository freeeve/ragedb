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
#include "../../src/gql/GqlExecutor.h"
#include "../../src/gql/executor/PathTraverser.h"

using namespace ragedb;
using namespace ragedb::gql;

/*
 * Task 021: LIMIT may only bound the physical scan when every remaining predicate runs INSIDE
 * that scan. These cases cover predicates the original residual guard missed: inline property
 * filters on a downstream (non-anchor) node, RETURN DISTINCT, and multi-filter anchor scans.
 * Matching rows are always created AFTER non-matching ones so a pushed-down limit scans only
 * non-matching rows and under-returns.
 */

static size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    size_t count = 0, pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) { count++; pos += needle.size(); }
    return count;
}

TEST_CASE("LIMIT pushdown residual gaps", "[gql_executor_limit][task021]") {
    auto graph = Graph("gql_limit_pushdown_task021");
    graph.Start().get();
    graph.Clear();
    graph.shard.local().NodeTypeInsertPeered("Person").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "name", "string").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "city", "string").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "age", "integer").get();
    graph.shard.local().RelationshipTypeInsertPeered("KNOWS").get();

    SECTION("inline property filter on the target node must not bound the anchor scan") {
        // 15 sources whose target is NOT named X first, then 5 whose target IS.
        for (int i = 0; i < 15; ++i) {
            uint64_t src = graph.shard.local().NodeAddPeered("Person", "miss_src" + std::to_string(i),
                "{\"name\": \"miss_src" + std::to_string(i) + "\", \"city\": \"A\", \"age\": 20}").get();
            uint64_t tgt = graph.shard.local().NodeAddPeered("Person", "miss_tgt" + std::to_string(i),
                "{\"name\": \"NotX\", \"city\": \"A\", \"age\": 20}").get();
            graph.shard.local().RelationshipAddPeered("KNOWS", src, tgt, "{}").get();
        }
        for (int i = 0; i < 5; ++i) {
            uint64_t src = graph.shard.local().NodeAddPeered("Person", "hit_src" + std::to_string(i),
                "{\"name\": \"hit_src" + std::to_string(i) + "\", \"city\": \"A\", \"age\": 20}").get();
            uint64_t tgt = graph.shard.local().NodeAddPeered("Person", "hit_tgt" + std::to_string(i),
                "{\"name\": \"X\", \"city\": \"A\", \"age\": 20}").get();
            graph.shard.local().RelationshipAddPeered("KNOWS", src, tgt, "{}").get();
        }

        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person)-[:KNOWS]->(b:Person {name: 'X'}) RETURN a.name AS an LIMIT 5")).get();
        INFO("result: " << res);
        REQUIRE(count_occurrences(res, "hit_src") == 5);
    }

    SECTION("RETURN DISTINCT with LIMIT must dedup over the full scan") {
        for (int i = 0; i < 10; ++i) {
            graph.shard.local().NodeAddPeered("Person", "pa" + std::to_string(i),
                "{\"name\": \"pa" + std::to_string(i) + "\", \"city\": \"A\", \"age\": 20}").get();
        }
        graph.shard.local().NodeAddPeered("Person", "pb", "{\"name\": \"pb\", \"city\": \"B\", \"age\": 20}").get();
        graph.shard.local().NodeAddPeered("Person", "pc", "{\"name\": \"pc\", \"city\": \"C\", \"age\": 20}").get();

        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (n:Person) RETURN DISTINCT n.city AS city LIMIT 3")).get();
        INFO("result: " << res);
        REQUIRE(count_occurrences(res, "\"city\":") == 3);
    }

    SECTION("multi-filter anchor scan must not truncate per-filter lists before intersecting") {
        for (int i = 0; i < 10; ++i) {
            graph.shard.local().NodeAddPeered("Person", "m30a" + std::to_string(i),
                "{\"name\": \"m30a" + std::to_string(i) + "\", \"city\": \"A\", \"age\": 30}").get();
        }
        for (int i = 0; i < 10; ++i) {
            graph.shard.local().NodeAddPeered("Person", "m40b" + std::to_string(i),
                "{\"name\": \"m40b" + std::to_string(i) + "\", \"city\": \"B\", \"age\": 40}").get();
        }
        graph.shard.local().NodeAddPeered("Person", "target",
            "{\"name\": \"target\", \"city\": \"B\", \"age\": 30}").get();

        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (n:Person {age: 30, city: 'B'}) RETURN n.name AS nn LIMIT 1")).get();
        INFO("result: " << res);
        REQUIRE(res.find("target") != std::string::npos);
    }

    graph.Stop().get();
}

TEST_CASE("chunked start-node scan matches one-shot scan results", "[gql_executor_limit][task020]") {
    auto graph = Graph("gql_chunked_scan_task020");
    graph.Start().get();
    graph.Clear();
    graph.shard.local().NodeTypeInsertPeered("Person").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "name", "string").get();
    graph.shard.local().RelationshipTypeInsertPeered("KNOWS").get();

    // 12 persons; only the even-indexed ones point at p0, so odd anchors yield no match and a
    // LIMIT-bounded anchor scan would under-return without scanning past them.
    std::vector<uint64_t> ids;
    for (int i = 0; i < 12; ++i) {
        ids.push_back(graph.shard.local().NodeAddPeered("Person", "p" + std::to_string(i),
            "{\"name\": \"p" + std::to_string(i) + "\"}").get());
    }
    for (int i = 2; i < 12; i += 2) {
        graph.shard.local().RelationshipAddPeered("KNOWS", ids[i], ids[0], "{}").get();
    }

    auto count_rows = [](const std::string& r) {
        size_t c = 0, pos = 0;
        while ((pos = r.find("\"an\":", pos)) != std::string::npos) { c++; pos += 5; }
        return c;
    };

    const size_t saved_chunk = gql_scan_chunk_size;

    SECTION("full result set is identical across chunk boundaries") {
        std::string full = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person)-[:KNOWS]->(b:Person) RETURN a.name AS an")).get();
        gql_scan_chunk_size = 4;
        std::string chunked = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person)-[:KNOWS]->(b:Person) RETURN a.name AS an")).get();
        gql_scan_chunk_size = saved_chunk;
        INFO("full: " << full << " chunked: " << chunked);
        REQUIRE(count_rows(chunked) == 5);
        REQUIRE(chunked == full);
    }

    SECTION("LIMIT stops the paged scan once satisfied without under-returning") {
        gql_scan_chunk_size = 3;
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person)-[:KNOWS]->(b:Person) RETURN a.name AS an LIMIT 4")).get();
        gql_scan_chunk_size = saved_chunk;
        INFO("result: " << res);
        REQUIRE(count_rows(res) == 4);
    }

    graph.Stop().get();
}
