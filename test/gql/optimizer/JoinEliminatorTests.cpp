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
#include <set>
#include <string>
#include "../../../src/gql/GqlParser.h"
#include "../../../src/gql/GqlOptimizer.h"
#include "../../../src/gql/GqlVirtualCatalog.h"
#include "../../../src/gql/optimizer/OptimizerUtils.h"

using namespace ragedb;
using namespace ragedb::gql;

TEST_CASE("GQL Optimizer Phase 2: Join Elimination", "[gql_optimizer][semantic]") {
    GqlVirtualCatalog::local().clear();
    // Register mandatory relationship constraint: every Shipment must have a SHIPPED_FROM Location
    GqlVirtualCatalog::local().add_constraint("MandatoryShippedFrom", "MATCH (s:Shipment) WHERE NOT EXISTS { MATCH (s)-[:SHIPPED_FROM]->(l:Location) } RETURN s");

    // DISTINCT: the mandatory edge is at-least-one, not exactly-one, so stripping it is only sound under
    // set semantics (bag/count would lose the rows a multi-edge Shipment multiplies in).
    std::string query_str = "MATCH (s:Shipment)-[:SHIPPED_FROM]->(l:Location) RETURN DISTINCT s";
    auto query = GqlParser::parse(query_str);
    GqlOptimizer::optimize(query);

    // The join should be eliminated: the pattern should contain only the start node and no edges
    REQUIRE(query.matches[0].pattern.nodes.size() == 1);
    REQUIRE(query.matches[0].pattern.nodes[0].variable == "s");
    REQUIRE(query.matches[0].pattern.edges.empty());

    GqlVirtualCatalog::local().clear();
}

TEST_CASE("join elimination does not strip a mandatory edge under bag or aggregate semantics", "[gql_optimizer]") {
    // The mandatory constraint guarantees at least one SHIPPED_FROM, not exactly one. Stripping the edge
    // for a non-DISTINCT projection or a count(*) would undercount a Shipment that ships from several
    // locations, so both must keep the two-node pattern.
    GqlVirtualCatalog::local().clear();
    GqlVirtualCatalog::local().add_constraint(
        "MandatoryShippedFrom",
        "MATCH (s:Shipment) WHERE NOT EXISTS { MATCH (s)-[:SHIPPED_FROM]->(l:Location) } RETURN s");

    SECTION("bag projection keeps the edge") {
        auto q = GqlParser::parse("MATCH (s:Shipment)-[:SHIPPED_FROM]->(l:Location) RETURN s");
        GqlOptimizer::optimize(q);
        REQUIRE(q.matches[0].pattern.nodes.size() == 2);
        REQUIRE_FALSE(q.matches[0].pattern.edges.empty());
    }

    SECTION("count(*) keeps the edge (sum(deg) vs count(s) would differ)") {
        auto q = GqlParser::parse("MATCH (s:Shipment)-[:SHIPPED_FROM]->(l:Location) RETURN count(*) AS n");
        GqlOptimizer::optimize(q);
        REQUIRE(q.matches[0].pattern.nodes.size() == 2);
        REQUIRE_FALSE(q.matches[0].pattern.edges.empty());
    }

    GqlVirtualCatalog::local().clear();
}

TEST_CASE("join elimination does not crash on count(*) over a mandatory relation", "[gql_optimizer]") {
    // is_var_referenced_in_query_except_match walks the RETURN expressions; count(*) is an aggregate
    // with a NULL argument, and the walk pushed that null child and dereferenced it (the 092 pattern,
    // copy-pasted here without the guard). Reaching the walk needs the mandatory-relation branch, so the
    // constraint is registered and the pattern matches it.
    GqlVirtualCatalog::local().clear();
    GqlVirtualCatalog::local().add_constraint(
        "MandatoryShippedFrom",
        "MATCH (s:Shipment) WHERE NOT EXISTS { MATCH (s)-[:SHIPPED_FROM]->(l:Location) } RETURN s");

    auto query = GqlParser::parse("MATCH (s:Shipment)-[:SHIPPED_FROM]->(l:Location) RETURN count(*) AS n");
    // Must not segfault.
    GqlOptimizer::optimize(query);
    SUCCEED("optimize returned without crashing");

    GqlVirtualCatalog::local().clear();
}

TEST_CASE("join elimination keeps a match whose variable the projection still uses", "[gql_optimizer]") {
    // Stripping the join is only invisible when nothing else needs the target variable. The reference
    // check has to see every spelling: one it fails to notice reads as "unused", and the match is erased
    // while the projection still asks for the value.
    auto still_bound = [](const std::string& projection) {
        GqlVirtualCatalog::local().clear();
        GqlVirtualCatalog::local().add_constraint(
            "MandatoryShippedFrom",
            "MATCH (s:Shipment) WHERE NOT EXISTS { MATCH (s)-[:SHIPPED_FROM]->(l:Location) } RETURN s");
        auto query = GqlParser::parse(
            "MATCH (s:Shipment)-[:SHIPPED_FROM]->(l:Location) RETURN DISTINCT " + projection);
        GqlOptimizer::optimize(query);
        std::set<std::string> bound;
        collect_variables_from_matches(query.matches, bound);
        GqlVirtualCatalog::local().clear();
        return bound.count("l") == 1;
    };

    SECTION("references the check already saw") {
        REQUIRE(still_bound("l.name"));
        REQUIRE(still_bound("l.name IN ['a', 'b']"));
    }

    SECTION("references it used to miss") {
        REQUIRE(still_bound("upper(l.name)"));
        REQUIRE(still_bound("CASE WHEN l.name = 'x' THEN 1 ELSE 0 END"));
        REQUIRE(still_bound("CAST(l.code AS INTEGER)"));
        REQUIRE(still_bound("[l.name]"));
        REQUIRE(still_bound("l.name IS NULL"));
    }

    SECTION("a genuinely unused target is still eliminated") {
        // Without this the fix could pass by never eliminating anything.
        REQUIRE_FALSE(still_bound("s.id"));
    }
}
