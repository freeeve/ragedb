// TEMPORARY crash-repro probe. Each variant is its own TEST_CASE so it can be run in an isolated
// process; a SIGSEGV in one then cannot mask the others. Not part of the committed suite.
#include <catch2/catch.hpp>
#include <Graph.h>
#include "../../src/gql/GqlParser.h"
#include "../../src/gql/GqlOptimizer.h"
#include "../../src/gql/GqlExecutor.h"

using namespace ragedb;
using namespace ragedb::gql;

static Graph make_graph() {
    Graph graph("gql_star_crash_probe");
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
    return graph;
}
static std::string run(Graph& g, const std::string& q){ auto qq=GqlParser::parse(q); GqlOptimizer::optimize(qq); return GqlExecutor::execute(g, std::move(qq)).get(); }

TEST_CASE("probe A: 3 branches, repeated rel type, count(*)", "[crashprobe]") {
    auto g=make_graph(); INFO(run(g,"MATCH (h:Person)-[:KNOWS]->(f:Person) MATCH (h)-[:LIKES]->(m:Person) MATCH (h)-[:KNOWS]->(x:Person) RETURN count(*) AS n")); g.Stop().get(); SUCCEED();
}
TEST_CASE("probe B: 3 branches, three distinct rel types, count(*)", "[crashprobe]") {
    auto g=make_graph(); INFO(run(g,"MATCH (h:Person)-[:KNOWS]->(f:Person) MATCH (h)-[:LIKES]->(m:Person) MATCH (h)-[:LIKES]->(x:Person) RETURN count(*) AS n")); g.Stop().get(); SUCCEED();
}
TEST_CASE("probe C: 3 branches, repeated rel type, RETURN vars not count", "[crashprobe]") {
    auto g=make_graph(); INFO(run(g,"MATCH (h:Person)-[:KNOWS]->(f:Person) MATCH (h)-[:LIKES]->(m:Person) MATCH (h)-[:KNOWS]->(x:Person) RETURN h.name AS hn")); g.Stop().get(); SUCCEED();
}
TEST_CASE("probe D: 2 branches repeated rel type (KNOWS twice), count(*)", "[crashprobe]") {
    auto g=make_graph(); INFO(run(g,"MATCH (h:Person)-[:KNOWS]->(f:Person) MATCH (h)-[:KNOWS]->(x:Person) RETURN count(*) AS n")); g.Stop().get(); SUCCEED();
}
TEST_CASE("probe E: 4 branches KNOWS LIKES KNOWS LIKES, count(*)", "[crashprobe]") {
    auto g=make_graph(); INFO(run(g,"MATCH (h:Person)-[:KNOWS]->(a:Person) MATCH (h)-[:LIKES]->(b:Person) MATCH (h)-[:KNOWS]->(c:Person) MATCH (h)-[:LIKES]->(d:Person) RETURN count(*) AS n")); g.Stop().get(); SUCCEED();
}
