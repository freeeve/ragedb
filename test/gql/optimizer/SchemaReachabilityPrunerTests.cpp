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

// SchemaReachabilityPruner answers a query with nothing when its edges contradict the registered schema,
// so the boundary of what the schema actually claims decides whether an empty result is right. Registered
// rules are a whitelist for the relationship types they mention; they are not an assertion that the
// schema describes every type in the graph. These pin both sides of that line, since a false positive
// here is indistinguishable from "no matches".

#include <catch2/catch.hpp>
#include <string>
#include "../../../src/gql/GqlParser.h"
#include "../../../src/gql/GqlOptimizer.h"
#include "../../../src/gql/GqlVirtualCatalog.h"

using namespace ragedb;
using namespace ragedb::gql;

namespace {
bool pruned(const std::string& text) {
    auto query = GqlParser::parse(text);
    GqlOptimizer::optimize(query);
    return query.no_op;
}
}  // namespace

TEST_CASE("schema reachability prunes only types the schema describes", "[gql_optimizer]") {
    GqlVirtualCatalog::local().clear();
    GqlVirtualCatalog::local().add_allowed_relationship("Person", "FRIEND", "Person");

    SECTION("an edge matching the rule survives") {
        REQUIRE_FALSE(pruned("MATCH (a:Person)-[:FRIEND]->(b:Person) RETURN a"));
    }

    SECTION("a described type used against the rule is unsatisfiable") {
        // FRIEND is declared to run Person -> Person, so a FRIEND edge to any other label contradicts
        // the declaration and cannot match.
        REQUIRE(pruned("MATCH (a:Person)-[:FRIEND]->(b:Category) RETURN a"));
    }

    SECTION("a type the schema never mentions is not judged") {
        // Declaring a rule for FRIEND says nothing about LIKES or HAS_ITEM. Treating that silence as a
        // denial emptied queries with real answers.
        REQUIRE_FALSE(pruned("MATCH (a:Person)-[:LIKES]->(b:Person) RETURN a"));
        REQUIRE_FALSE(pruned("MATCH (o:Order)-[:HAS_ITEM]->(i:Item) RETURN o"));
    }

    GqlVirtualCatalog::local().clear();
}

TEST_CASE("an undirected edge is judged against both orientations", "[gql_optimizer]") {
    // `-[:R]-` matches an edge traversed either way, so it contradicts the schema only when NEITHER
    // orientation is declared. Checking one direction would empty queries whose edges exist, just written
    // from the other end.
    GqlVirtualCatalog::local().clear();
    GqlVirtualCatalog::local().add_allowed_relationship("Person", "FRIEND", "Person");
    GqlVirtualCatalog::local().add_allowed_relationship("Person", "WORKS_AT", "Company");

    SECTION("the declared orientation survives") {
        REQUIRE_FALSE(pruned("MATCH (a:Person)-[:WORKS_AT]-(b:Company) RETURN a"));
    }

    SECTION("the reverse of a declared orientation also survives") {
        // The schema declares Person -WORKS_AT-> Company; written from the Company end an undirected
        // edge is the same set of edges, so it must not be judged impossible.
        REQUIRE_FALSE(pruned("MATCH (a:Company)-[:WORKS_AT]-(b:Person) RETURN a"));
    }

    SECTION("neither orientation declared is unsatisfiable") {
        REQUIRE(pruned("MATCH (a:Company)-[:WORKS_AT]-(b:Company) RETURN a"));
    }

    SECTION("a type the schema never mentions is not judged, undirected either") {
        // Same rule as for a directed edge: registered rules are a whitelist for the types they name,
        // and silence about a type is not a denial.
        REQUIRE_FALSE(pruned("MATCH (a:Person)-[:LIKES]-(b:Company) RETURN a"));
    }

    GqlVirtualCatalog::local().clear();
}

TEST_CASE("schema reachability prunes nothing without a registered schema", "[gql_optimizer]") {
    GqlVirtualCatalog::local().clear();
    REQUIRE_FALSE(pruned("MATCH (a:Person)-[:FRIEND]->(b:Category) RETURN a"));
}
