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

using namespace ragedb;
using namespace ragedb::gql;

/*
 * WITH-pipeline correctness (task 019): continuation segments carry piped rows, so any planner
 * that answers from indexes or re-runs the pattern from scratch must be skipped for them, and an
 * empty segment must still flow into a later ungrouped aggregate (which yields count = 0).
 *
 * Data: Alice/Bob/Carol form a KNOWS triangle; Dave/Erin/Frank form a second, disjoint triangle.
 * Two Comment nodes exist for the count fast-path checks.
 */
static void populate_with_pipeline_graph(Graph& graph) {
    graph.Clear();
    graph.shard.local().NodeTypeInsertPeered("Person").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "name", "string").get();
    graph.shard.local().NodeTypeInsertPeered("Comment").get();
    graph.shard.local().NodePropertyTypeAddPeered("Comment", "score", "integer").get();
    graph.shard.local().RelationshipTypeInsertPeered("KNOWS").get();

    auto add_person = [&](const std::string& key, const std::string& name) {
        return graph.shard.local().NodeAddPeered("Person", key, "{\"name\": \"" + name + "\"}").get();
    };
    uint64_t alice = add_person("alice", "Alice");
    uint64_t bob = add_person("bob", "Bob");
    uint64_t carol = add_person("carol", "Carol");
    uint64_t dave = add_person("dave", "Dave");
    uint64_t erin = add_person("erin", "Erin");
    uint64_t frank = add_person("frank", "Frank");

    auto knows = [&](uint64_t from, uint64_t to) {
        graph.shard.local().RelationshipAddPeered("KNOWS", from, to, "{}").get();
    };
    knows(alice, bob); knows(bob, carol); knows(carol, alice);
    knows(dave, erin); knows(erin, frank); knows(frank, dave);

    graph.shard.local().NodeAddPeered("Comment", "c1", "{\"score\": 1}").get();
    graph.shard.local().NodeAddPeered("Comment", "c2", "{\"score\": 2}").get();
}

TEST_CASE("WITH continuation segments must consume piped rows", "[gql_executor_with][task019]") {
    auto graph = Graph("gql_with_piped_rows_test");
    graph.Start().get();
    populate_with_pipeline_graph(graph);

    SECTION("count fast path must not fire on a continuation segment: empty pipe means count 0") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (p:Person {name: 'Nobody'}) WITH p MATCH (c:Comment) RETURN count(c) AS n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": 0") != std::string::npos);
        REQUIRE(res.find("\"n\": 2") == std::string::npos);
    }

    SECTION("count after a non-empty pipe is per-row (cartesian), not the global label count") {
        // 6 piped persons x 2 comments = 12, not 2.
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (p:Person) WITH p MATCH (c:Comment) RETURN count(c) AS n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": 12") != std::string::npos);
    }

    SECTION("cyclic pattern after WITH is anchored to the piped binding, not every cycle") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person {name: 'Alice'}) WITH a "
                        "MATCH (a)-[:KNOWS]->(b)-[:KNOWS]->(c)-[:KNOWS]->(a) "
                        "RETURN b.name AS bn, c.name AS cn")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"bn\": \"Bob\", \"cn\": \"Carol\"") != std::string::npos);
        // The disjoint Dave/Erin/Frank triangle must not appear.
        REQUIRE(res.find("Dave") == std::string::npos);
        REQUIRE(res.find("Erin") == std::string::npos);
        REQUIRE(res.find("Frank") == std::string::npos);
    }

    SECTION("an empty segment still reaches a later ungrouped aggregate: one row with count 0") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person {name: 'Nobody'}) WITH a "
                        "MATCH (a)-[:KNOWS]->(b) WITH count(b) AS n RETURN n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": 0") != std::string::npos);
    }

    SECTION("edge-expansion count after WITH is anchored to the piped node") {
        // Alice has exactly one outgoing KNOWS edge (to Bob); the count->degree-sum rewrite must
        // not fire on the continuation part (its anchor is piped in, never degree-populated).
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person {name: 'Alice'}) WITH a "
                        "MATCH (a)-[:KNOWS]->(b) RETURN count(b) AS n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": 1") != std::string::npos);
    }

    graph.Stop().get();
}

TEST_CASE("count over an empty expansion is 0 even when rewritten to a degree sum", "[gql_executor_with][task019]") {
    auto graph = Graph("gql_count_to_sum_empty_test");
    graph.Start().get();
    graph.Clear();
    graph.shard.local().NodeTypeInsertPeered("Person").get();
    graph.shard.local().RelationshipTypeInsertPeered("KNOWS").get();

    // No Person nodes at all: the degree-sum rewrite fires, but the result must stay
    // count-shaped (0), not sum-shaped (null).
    std::string res = GqlExecutor::execute(graph,
        std::string("MATCH (a:Person)-[:KNOWS]->(b) RETURN count(b) AS n")).get();
    INFO("result: " << res);
    REQUIRE(res.find("\"n\": 0") != std::string::npos);

    graph.Stop().get();
}

TEST_CASE("streaming edge aggregate rejects unsupported shapes", "[gql_executor_with][task019]") {
    auto graph = Graph("gql_streaming_guard_test");
    graph.Start().get();
    graph.Clear();
    graph.shard.local().NodeTypeInsertPeered("Person").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "name", "string").get();
    graph.shard.local().RelationshipTypeInsertPeered("KNOWS").get();
    uint64_t a = graph.shard.local().NodeAddPeered("Person", "alice", "{\"name\": \"Alice\"}").get();
    uint64_t b = graph.shard.local().NodeAddPeered("Person", "bob", "{\"name\": \"Bob\"}").get();
    // One self-loop (Alice) and two ordinary edges.
    graph.shard.local().RelationshipAddPeered("KNOWS", a, a, "{}").get();
    graph.shard.local().RelationshipAddPeered("KNOWS", a, b, "{}").get();
    graph.shard.local().RelationshipAddPeered("KNOWS", b, a, "{}").get();

    SECTION("self-loop pattern counts only self-loops, not every edge") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (x:Person)-[:KNOWS]->(x) RETURN count(x) AS n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": 1") != std::string::npos);
        REQUIRE(res.find("\"n\": 3") == std::string::npos);
    }

    graph.Stop().get();
}
