// TEMPORARY isolation probe (tag [starx]). Each TEST_CASE is run in its own process. Goal: separate
// "the three-branch query crashes the executor" from "a second Graph lifecycle in one process crashes".
// Graphs are constructed in place with the SAME name across sections, matching the committed test
// (not by-value like the earlier probe), so the construction pattern is not a confounder.
#include <catch2/catch.hpp>
#include <Graph.h>
#include "../../src/gql/GqlParser.h"
#include "../../src/gql/GqlOptimizer.h"
#include "../../src/gql/GqlExecutor.h"

using namespace ragedb;
using namespace ragedb::gql;

namespace {
struct SG { Graph& g; ~SG() { g.Stop().get(); } };

void seed(Graph& graph) {
    graph.Start().get();
    graph.Clear();
    graph.shard.local().NodeTypeInsertPeered("Person").get();
    graph.shard.local().NodePropertyTypeAddPeered("Person", "name", "string").get();
    graph.shard.local().RelationshipTypeInsertPeered("KNOWS").get();
    graph.shard.local().RelationshipTypeInsertPeered("LIKES").get();
    auto p = [&](const std::string& n){ return graph.shard.local().NodeAddPeered("Person", n, "{\"name\":\"" + n + "\"}").get(); };
    uint64_t ha=p("HubA"), hb=p("HubB"), f1=p("F1"), f2=p("F2"), f3=p("F3"), i1=p("I1"), i2=p("I2"), i3=p("I3");
    auto r=[&](const char* t, uint64_t a, uint64_t b){ graph.shard.local().RelationshipAddPeered(t,a,b,"{}").get(); };
    r("KNOWS",ha,f1); r("KNOWS",ha,f2); r("LIKES",ha,i1);
    r("KNOWS",hb,f3); r("LIKES",hb,i2); r("LIKES",hb,i3);
}
std::string run(Graph& g, const std::string& q){ auto qq=GqlParser::parse(q); GqlOptimizer::optimize(qq); return GqlExecutor::execute(g, std::move(qq)).get(); }

const char* Q3 = "MATCH (h:Person)-[:KNOWS]->(f:Person) MATCH (h)-[:LIKES]->(m:Person) MATCH (h)-[:KNOWS]->(g:Person) RETURN count(*) AS n";
const char* Q2 = "MATCH (h:Person)-[:KNOWS]->(f:Person) MATCH (h)-[:LIKES]->(m:Person) RETURN count(*) AS n";
}  // namespace

TEST_CASE("T1 three-branch as the sole graph lifecycle", "[starx]") {
    Graph graph("starx"); SG sg{graph}; seed(graph);
    INFO(run(graph, Q3)); SUCCEED();
}
TEST_CASE("T2 two-branch section then three-branch section", "[starx]") {
    SECTION("a") { Graph graph("starx"); SG sg{graph}; seed(graph); INFO(run(graph, Q2)); SUCCEED(); }
    SECTION("b") { Graph graph("starx"); SG sg{graph}; seed(graph); INFO(run(graph, Q3)); SUCCEED(); }
}
TEST_CASE("T3 three-branch twice", "[starx]") {
    SECTION("a") { Graph graph("starx"); SG sg{graph}; seed(graph); INFO(run(graph, Q3)); SUCCEED(); }
    SECTION("b") { Graph graph("starx"); SG sg{graph}; seed(graph); INFO(run(graph, Q3)); SUCCEED(); }
}
TEST_CASE("T4 two-branch twice", "[starx]") {
    SECTION("a") { Graph graph("starx"); SG sg{graph}; seed(graph); INFO(run(graph, Q2)); SUCCEED(); }
    SECTION("b") { Graph graph("starx"); SG sg{graph}; seed(graph); INFO(run(graph, Q2)); SUCCEED(); }
}
