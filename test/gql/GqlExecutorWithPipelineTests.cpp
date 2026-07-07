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

TEST_CASE("NEXT continuation segments must consume piped rows", "[gql_executor_with][task019]") {
    auto graph = Graph("gql_with_piped_rows_test");
    graph.Start().get();
    populate_with_pipeline_graph(graph);

    SECTION("count fast path must not fire on a continuation segment: empty pipe means count 0") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (p:Person {name: 'Nobody'}) RETURN p NEXT MATCH (c:Comment) RETURN count(c) AS n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": 0") != std::string::npos);
        REQUIRE(res.find("\"n\": 2") == std::string::npos);
    }

    SECTION("count after a non-empty pipe is per-row (cartesian), not the global label count") {
        // 6 piped persons x 2 comments = 12, not 2.
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (p:Person) RETURN p NEXT MATCH (c:Comment) RETURN count(c) AS n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": 12") != std::string::npos);
    }

    SECTION("cyclic pattern after NEXT is anchored to the piped binding, not every cycle") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person {name: 'Alice'}) RETURN a NEXT "
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
            std::string("MATCH (a:Person {name: 'Nobody'}) RETURN a NEXT "
                        "MATCH (a)-[:KNOWS]->(b) RETURN count(b) AS n NEXT RETURN n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": 0") != std::string::npos);
    }

    SECTION("edge-expansion count after NEXT is anchored to the piped node") {
        // Alice has exactly one outgoing KNOWS edge (to Bob); the count->degree-sum rewrite must
        // not fire on the continuation part (its anchor is piped in, never degree-populated).
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person {name: 'Alice'}) RETURN a NEXT "
                        "MATCH (a)-[:KNOWS]->(b) RETURN count(b) AS n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": 1") != std::string::npos);
    }

    graph.Stop().get();
}

TEST_CASE("streamed top-K and group folds match materialised results", "[gql_executor_with][task018][stream]") {
    auto graph = Graph("gql_streaming_fold_test");
    graph.Start().get();
    graph.Clear();
    graph.shard.local().NodeTypeInsertPeered("Person").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "name", "string").get();
    graph.shard.local().NodeTypeInsertPeered("Comment").get();
    graph.shard.local().NodePropertyTypeAddPeered("Comment", "score", "integer").get();
    graph.shard.local().RelationshipTypeInsertPeered("KNOWS").get();
    graph.shard.local().RelationshipTypeInsertPeered("HAS_CREATOR").get();

    uint64_t alice = graph.shard.local().NodeAddPeered("Person", "alice", "{\"name\": \"Alice\"}").get();
    uint64_t bob = graph.shard.local().NodeAddPeered("Person", "bob", "{\"name\": \"Bob\"}").get();
    uint64_t carol = graph.shard.local().NodeAddPeered("Person", "carol", "{\"name\": \"Carol\"}").get();
    uint64_t dave = graph.shard.local().NodeAddPeered("Person", "dave", "{\"name\": \"Dave\"}").get();
    graph.shard.local().RelationshipAddPeered("KNOWS", alice, bob, "{}").get();
    graph.shard.local().RelationshipAddPeered("KNOWS", alice, carol, "{}").get();
    graph.shard.local().RelationshipAddPeered("KNOWS", dave, bob, "{}").get();

    auto comment = [&](const std::string& key, int64_t score, uint64_t creator) {
        uint64_t c = graph.shard.local().NodeAddPeered("Comment", key,
            "{\"score\": " + std::to_string(score) + "}").get();
        graph.shard.local().RelationshipAddPeered("HAS_CREATOR", c, creator, "{}").get();
    };
    // Bob: scores 1, 5, 1 (three comments, two distinct scores); Carol: 2, 4.
    comment("b1", 1, bob); comment("b2", 5, bob); comment("b3", 1, bob);
    comment("c1", 2, carol); comment("c2", 4, carol);

    const size_t saved = gql_stream_chunk_size;
    gql_stream_chunk_size = 2;  // force multi-chunk streaming on this tiny graph

    SECTION("IC2 shape: RETURN ... ORDER BY ... LIMIT NEXT over a two-hop expansion (top-K)") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person {name: 'Alice'})-[:KNOWS]-(f:Person)<-[:HAS_CREATOR]-(m:Comment) "
                        "WHERE m.score > 1 RETURN m ORDER BY m.score DESC LIMIT 2 NEXT RETURN m.score AS s")).get();
        INFO("result: " << res);
        // Friend comments with score > 1: {5, 2, 4}; top 2 descending = 5, 4.
        REQUIRE(res.find("[{\"s\": 5}, {\"s\": 4}]") != std::string::npos);
    }

    SECTION("RETURN DISTINCT then expand then count (the crash shape, group fold)") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person {name: 'Alice'})-[:KNOWS]-(f:Person) RETURN DISTINCT f "
                        "NEXT MATCH (f)<-[:HAS_CREATOR]-(m:Comment) RETURN count(m) AS n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": 5") != std::string::npos);
    }

    SECTION("grouped fold over a two-hop expansion with DISTINCT aggregate and ORDER BY") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person {name: 'Alice'})-[:KNOWS]-(f:Person)<-[:HAS_CREATOR]-(m:Comment) "
                        "RETURN f.name AS fn, count(m) AS n, count(DISTINCT m.score) AS d "
                        "ORDER BY count(m) DESC")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"fn\": \"Bob\", \"n\": 3, \"d\": 2") != std::string::npos);
        REQUIRE(res.find("\"fn\": \"Carol\", \"n\": 2, \"d\": 2") != std::string::npos);
        REQUIRE(res.find("Bob") < res.find("Carol"));
    }

    SECTION("streamed results equal materialised results (no LIMIT control)") {
        // Without a LIMIT the top-K gate does not fire, so this exercises the ordinary path on
        // the same data; the LIMIT 3 variant must be its prefix.
        std::string full = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person {name: 'Alice'})-[:KNOWS]-(f:Person)<-[:HAS_CREATOR]-(m:Comment) "
                        "RETURN m ORDER BY m.score DESC NEXT RETURN m.score AS s")).get();
        std::string streamed = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person {name: 'Alice'})-[:KNOWS]-(f:Person)<-[:HAS_CREATOR]-(m:Comment) "
                        "RETURN m ORDER BY m.score DESC LIMIT 3 NEXT RETURN m.score AS s")).get();
        INFO("full: " << full << " streamed: " << streamed);
        REQUIRE(full.find("[{\"s\": 5}, {\"s\": 4}, {\"s\": 2}") != std::string::npos);
        REQUIRE(streamed.find("[{\"s\": 5}, {\"s\": 4}, {\"s\": 2}]") != std::string::npos);
    }

    gql_stream_chunk_size = saved;
    graph.Stop().get();
}

TEST_CASE("ORDER BY over RETURN aliases executes correctly (task 027)", "[gql_executor_with][task027]") {
    auto graph = Graph("gql_orderby_alias_test");
    graph.Start().get();
    graph.Clear();
    graph.shard.local().NodeTypeInsertPeered("Person").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "name", "string").get();
    graph.shard.local().NodeTypeInsertPeered("Comment").get();
    graph.shard.local().RelationshipTypeInsertPeered("HAS_CREATOR").get();

    uint64_t alice = graph.shard.local().NodeAddPeered("Person", "alice", "{\"name\": \"Alice\"}").get();
    uint64_t bob = graph.shard.local().NodeAddPeered("Person", "bob", "{\"name\": \"Bob\"}").get();
    uint64_t carol = graph.shard.local().NodeAddPeered("Person", "carol", "{\"name\": \"Carol\"}").get();
    auto comment = [&](const std::string& key, uint64_t creator) {
        uint64_t c = graph.shard.local().NodeAddPeered("Comment", key, "{}").get();
        graph.shard.local().RelationshipAddPeered("HAS_CREATOR", c, creator, "{}").get();
    };
    comment("c1", alice); comment("c2", alice); comment("c3", alice);
    comment("c4", bob);
    comment("c5", carol); comment("c6", carol);

    SECTION("sort by projected aggregate alias, then by projected name alias") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (p:Person)<-[:HAS_CREATOR]-(m:Comment) "
                        "RETURN p.name AS name, count(DISTINCT m) AS cnt "
                        "ORDER BY cnt DESC, name ASC LIMIT 2")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"name\": \"Alice\", \"cnt\": 3") != std::string::npos);
        REQUIRE(res.find("\"name\": \"Carol\", \"cnt\": 2") != std::string::npos);
        REQUIRE(res.find("Bob") == std::string::npos);
        REQUIRE(res.find("Alice") < res.find("Carol"));
    }

    SECTION("alias sort keys work in an intermediate NEXT segment") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (p:Person)<-[:HAS_CREATOR]-(m:Comment) "
                        "RETURN p, count(m) AS cnt ORDER BY cnt DESC LIMIT 1 "
                        "NEXT RETURN p.name AS n, cnt")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": \"Alice\", \"cnt\": 3") != std::string::npos);
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

TEST_CASE("GQL expression forms execute (task 032: CASE/length/zoned_datetime/COUNT{}/collect_list/IN)",
          "[gql_executor_with][task032]") {
    auto graph = Graph("gql_expr_forms_test");
    graph.Start().get();
    graph.Clear();
    graph.shard.local().NodeTypeInsertPeered("Person").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "name", "string").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "created", "integer").get();
    graph.shard.local().RelationshipTypeInsertPeered("KNOWS").get();
    // created is epoch MILLISECONDS: Alice 2011-06-01 (1306886400000), Bob 2010-06-01 (1275350400000).
    uint64_t alice = graph.shard.local().NodeAddPeered("Person", "alice", "{\"name\": \"Alice\", \"created\": 1306886400000}").get();
    uint64_t bob   = graph.shard.local().NodeAddPeered("Person", "bob",   "{\"name\": \"Bob\", \"created\": 1275350400000}").get();
    uint64_t carol = graph.shard.local().NodeAddPeered("Person", "carol", "{\"name\": \"Carol\", \"created\": 1306886400000}").get();
    graph.shard.local().RelationshipAddPeered("KNOWS", alice, bob, "{}").get();
    graph.shard.local().RelationshipAddPeered("KNOWS", bob, carol, "{}").get();
    graph.shard.local().RelationshipAddPeered("KNOWS", alice, carol, "{}").get();

    SECTION("sum(CASE WHEN ... THEN 1 ELSE 0 END)") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (p:Person) RETURN sum(CASE WHEN p.name = 'Alice' THEN 1 ELSE 0 END) AS n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"n\": 1") != std::string::npos);
    }

    SECTION("COUNT { } subquery-count of a friend expansion") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (p:Person {name: 'Alice'}) RETURN COUNT { (p)-[:KNOWS]->(f:Person) } AS c")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"c\": 2") != std::string::npos);  // Alice knows Bob and Carol
    }

    SECTION("length(path) over a shortest path is the hop count") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH p = ANY SHORTEST (a)-[:KNOWS]-{1,3}(b) "
                        "WHERE a.name = 'Alice' AND b.name = 'Carol' RETURN length(p) AS d")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"d\": 1") != std::string::npos);  // Alice-[:KNOWS]->Carol is direct
    }

    SECTION("zoned_datetime() compares in epoch milliseconds (unit check)") {
        // created >= 2011-01-01 (1293840000000 ms). Alice/Carol are 2011-06 (match); Bob is 2010-06 (no).
        // If zoned_datetime returned SECONDS, all three would match -- so this discriminates the unit.
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (p:Person) WHERE p.created >= zoned_datetime('2011-01-01') "
                        "RETURN p.name AS n ORDER BY n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("Alice") != std::string::npos);
        REQUIRE(res.find("Carol") != std::string::npos);
        REQUIRE(res.find("Bob") == std::string::npos);
    }

    SECTION("collect_list then NOT (x IN before) membership across NEXT") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (a:Person {name: 'Alice'})-[:KNOWS]->(f:Person) "
                        "RETURN collect_list(f) AS before "
                        "NEXT "
                        "MATCH (x:Person) WHERE NOT (x IN before) RETURN x.name AS n")).get();
        INFO("result: " << res);
        REQUIRE(res.find("Alice") != std::string::npos);  // Alice is not among her own friends
        REQUIRE(res.find("Bob") == std::string::npos);
        REQUIRE(res.find("Carol") == std::string::npos);
    }

    graph.Stop().get();
}

TEST_CASE("multi-match streamed group fold matches the batch result (task 029)", "[gql_executor_with][task029]") {
    auto graph = Graph("gql_multimatch_stream_test");
    graph.Start().get();
    graph.Clear();
    graph.shard.local().NodeTypeInsertPeered("Person").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "id", "integer").get();
    graph.shard.local().NodeTypeInsertPeered("Forum").get();
    graph.shard.local().NodePropertyTypeAddPeered("Forum", "id", "integer").get();
    graph.shard.local().NodeTypeInsertPeered("Post").get();
    graph.shard.local().NodePropertyTypeAddPeered("Post", "id", "integer").get();
    graph.shard.local().RelationshipTypeInsertPeered("KNOWS").get();
    graph.shard.local().RelationshipTypeInsertPeered("HAS_MEMBER").get();
    graph.shard.local().RelationshipTypeInsertPeered("CONTAINER_OF").get();
    graph.shard.local().RelationshipTypeInsertPeered("HAS_CREATOR").get();
    uint64_t p1 = graph.shard.local().NodeAddPeered("Person", "1", "{\"id\": 1}").get();
    uint64_t f1 = graph.shard.local().NodeAddPeered("Person", "2", "{\"id\": 2}").get();
    graph.shard.local().RelationshipAddPeered("KNOWS", p1, f1, "{}").get();
    // forum10: 3 distinct posts by f1; forum20: 1 post by f1. Both have f1 as member.
    uint64_t forum10 = graph.shard.local().NodeAddPeered("Forum", "10", "{\"id\": 10}").get();
    uint64_t forum20 = graph.shard.local().NodeAddPeered("Forum", "20", "{\"id\": 20}").get();
    graph.shard.local().RelationshipAddPeered("HAS_MEMBER", forum10, f1, "{}").get();
    graph.shard.local().RelationshipAddPeered("HAS_MEMBER", forum20, f1, "{}").get();
    for (int i = 100; i < 103; ++i) {  // forum10 posts 100,101,102
        uint64_t post = graph.shard.local().NodeAddPeered("Post", std::to_string(i), "{\"id\": " + std::to_string(i) + "}").get();
        graph.shard.local().RelationshipAddPeered("CONTAINER_OF", forum10, post, "{}").get();
        graph.shard.local().RelationshipAddPeered("HAS_CREATOR", post, f1, "{}").get();
    }
    uint64_t post200 = graph.shard.local().NodeAddPeered("Post", "200", "{\"id\": 200}").get();  // forum20 post
    graph.shard.local().RelationshipAddPeered("CONTAINER_OF", forum20, post200, "{}").get();
    graph.shard.local().RelationshipAddPeered("HAS_CREATOR", post200, f1, "{}").get();

    const size_t saved = gql_stream_chunk_size;
    gql_stream_chunk_size = 1;  // force multi-chunk streaming through the 2-match chain

    // Piped frontier -> two-MATCH expansion -> grouped count(DISTINCT) with ORDER BY (IC5 shape).
    std::string res = GqlExecutor::execute(graph,
        std::string("MATCH (p:Person {id: 1})-[:KNOWS]-{1,2}(f:Person) WHERE f.id <> 1 "
                    "RETURN DISTINCT f "
                    "NEXT "
                    "MATCH (forum:Forum)-[:HAS_MEMBER]->(f) "
                    "MATCH (forum)-[:CONTAINER_OF]->(post:Post)-[:HAS_CREATOR]->(f) "
                    "RETURN forum.id AS forumId, count(DISTINCT post) AS postCount "
                    "ORDER BY postCount DESC, forumId ASC")).get();
    INFO("result: " << res);
    gql_stream_chunk_size = saved;

    REQUIRE(res.find("\"forumId\": 10, \"postCount\": 3") != std::string::npos);
    REQUIRE(res.find("\"forumId\": 20, \"postCount\": 1") != std::string::npos);
    REQUIRE(res.find("10") < res.find("20"));  // ordered by postCount DESC

    graph.Stop().get();
}

TEST_CASE("GQL IC5 shape executes without crashing (task 032 crash repro)", "[gql_executor_with][task032ic5]") {
    auto graph = Graph("gql_ic5_shape_test");
    graph.Start().get();
    graph.Clear();
    graph.shard.local().NodeTypeInsertPeered("Person").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "id", "integer").get();
    graph.shard.local().NodeTypeInsertPeered("Forum").get();
    graph.shard.local().NodePropertyTypeAddPeered("Forum", "id", "integer").get();
    graph.shard.local().NodeTypeInsertPeered("Post").get();
    graph.shard.local().NodePropertyTypeAddPeered("Post", "id", "integer").get();
    graph.shard.local().RelationshipTypeInsertPeered("KNOWS").get();
    graph.shard.local().RelationshipTypeInsertPeered("HAS_MEMBER").get();
    graph.shard.local().RelationshipTypeInsertPeered("CONTAINER_OF").get();
    graph.shard.local().RelationshipTypeInsertPeered("HAS_CREATOR").get();
    uint64_t p1 = graph.shard.local().NodeAddPeered("Person", "1", "{\"id\": 1}").get();
    uint64_t f1 = graph.shard.local().NodeAddPeered("Person", "2", "{\"id\": 2}").get();
    uint64_t f2 = graph.shard.local().NodeAddPeered("Person", "3", "{\"id\": 3}").get();
    uint64_t forum = graph.shard.local().NodeAddPeered("Forum", "10", "{\"id\": 10}").get();
    uint64_t post1 = graph.shard.local().NodeAddPeered("Post", "100", "{\"id\": 100}").get();
    uint64_t post2 = graph.shard.local().NodeAddPeered("Post", "101", "{\"id\": 101}").get();
    graph.shard.local().RelationshipAddPeered("KNOWS", p1, f1, "{}").get();
    graph.shard.local().RelationshipAddPeered("KNOWS", f1, f2, "{}").get();
    graph.shard.local().RelationshipAddPeered("HAS_MEMBER", forum, f1, "{}").get();
    graph.shard.local().RelationshipAddPeered("CONTAINER_OF", forum, post1, "{}").get();
    graph.shard.local().RelationshipAddPeered("CONTAINER_OF", forum, post2, "{}").get();
    graph.shard.local().RelationshipAddPeered("HAS_CREATOR", post1, f1, "{}").get();
    graph.shard.local().RelationshipAddPeered("HAS_CREATOR", post2, f1, "{}").get();

    SECTION("RETURN DISTINCT f NEXT (frontier only)") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (p:Person {id: 1})-[:KNOWS]-{1,2}(f:Person) WHERE f.id <> 1 "
                        "RETURN DISTINCT f NEXT RETURN f.id AS fid ORDER BY fid")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"fid\": 2") != std::string::npos);
    }
    SECTION("full IC5 shape") {
        std::string res = GqlExecutor::execute(graph,
            std::string("MATCH (p:Person {id: 1})-[:KNOWS]-{1,2}(f:Person) WHERE f.id <> 1 "
                        "RETURN DISTINCT f "
                        "NEXT "
                        "MATCH (forum:Forum)-[:HAS_MEMBER]->(f) "
                        "MATCH (forum)-[:CONTAINER_OF]->(post:Post)-[:HAS_CREATOR]->(f) "
                        "RETURN forum.id AS forumId, count(DISTINCT post) AS postCount "
                        "ORDER BY postCount DESC, forumId ASC LIMIT 5")).get();
        INFO("result: " << res);
        REQUIRE(res.find("\"forumId\": 10, \"postCount\": 2") != std::string::npos);
    }

    graph.Stop().get();
}
