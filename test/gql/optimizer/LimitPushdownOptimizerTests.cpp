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

// Limit pushdown turns a query LIMIT into a bound on the start-node scan. Every condition that stops
// it from firing exists because pushing anyway returns the wrong rows -- silently, since a truncated
// scan looks exactly like a small result. The pass had no correctness test of its own (only a timing
// benchmark that asserts nothing), so each guard is pinned here: what makes it fire, and what must
// stop it.

#include <catch2/catch.hpp>
#include "../../../src/gql/GqlParser.h"
#include "../../../src/gql/GqlOptimizer.h"
#include "../../../src/gql/GqlVirtualCatalog.h"

using namespace ragedb;
using namespace ragedb::gql;

namespace {
GqlQuery optimized(const std::string& text) {
    auto query = GqlParser::parse(text);
    GqlOptimizer::optimize(query);
    return query;
}
}  // namespace

TEST_CASE("limit pushdown bounds the scan on a plain single-match query", "[gql_optimizer]") {
    GqlVirtualCatalog::local().clear();
    auto q = optimized("MATCH (p:Person) RETURN p.name LIMIT 5");
    REQUIRE(q.matches.size() == 1);
    REQUIRE(q.matches[0].limit.has_value());
    REQUIRE(q.matches[0].limit.value() == 5);
}

TEST_CASE("limit pushdown pushes the whole page window, not the bare limit", "[gql_optimizer]") {
    GqlVirtualCatalog::local().clear();
    // An OFFSET skips past the first rows, so a scan bounded at the limit alone would stop before
    // reaching the rows the page is supposed to return.
    auto q = optimized("MATCH (p:Person) RETURN p.name OFFSET 10 LIMIT 5");
    REQUIRE(q.matches[0].limit.has_value());
    REQUIRE(q.matches[0].limit.value() == 15);
}

TEST_CASE("limit pushdown is refused when the result is ordered or folded", "[gql_optimizer]") {
    GqlVirtualCatalog::local().clear();

    SECTION("ORDER BY: the top-k rows are not the first rows scanned") {
        auto q = optimized("MATCH (p:Person) RETURN p.name ORDER BY p.name LIMIT 5");
        REQUIRE_FALSE(q.matches[0].limit.has_value());
    }

    SECTION("an aggregate bounds the RESULT, not the scan") {
        // Pushing here truncated the rows the aggregate folds over, so count(p) LIMIT 1 answered 1
        // instead of the count.
        auto q = optimized("MATCH (p:Person) RETURN count(p) AS n LIMIT 1");
        REQUIRE_FALSE(q.matches[0].limit.has_value());
    }

    SECTION("DISTINCT collapses rows after the scan") {
        auto q = optimized("MATCH (p:Person) RETURN DISTINCT p.name LIMIT 5");
        REQUIRE_FALSE(q.matches[0].limit.has_value());
    }

    SECTION("no LIMIT means nothing to push") {
        auto q = optimized("MATCH (p:Person) RETURN p.name");
        REQUIRE_FALSE(q.matches[0].limit.has_value());
    }
}

TEST_CASE("limit pushdown is refused when a predicate survives the scan", "[gql_optimizer]") {
    GqlVirtualCatalog::local().clear();

    SECTION("a segment WHERE filters rows the scan already counted") {
        auto q = optimized("MATCH (p:Person) WHERE p.age > 30 RETURN p.name LIMIT 5");
        REQUIRE_FALSE(q.matches[0].limit.has_value());
    }

    SECTION("an inline WHERE on a pattern node is the same hazard") {
        auto q = optimized("MATCH (p:Person WHERE p.age > 30) RETURN p.name LIMIT 5");
        REQUIRE_FALSE(q.matches[0].limit.has_value());
    }

    SECTION("a property filter on a non-anchor node post-filters") {
        auto q = optimized("MATCH (p:Person)-[:KNOWS]->(f:Person {name: 'Bob'}) RETURN p.name LIMIT 5");
        REQUIRE_FALSE(q.matches[0].limit.has_value());
    }
}

TEST_CASE("limit pushdown needs every join after the first to be mandatory", "[gql_optimizer]") {
    SECTION("an unconstrained second match may drop rows, so the scan stays unbounded") {
        GqlVirtualCatalog::local().clear();
        auto q = optimized(
            "MATCH (p:Person) MATCH (p)-[:SHIPPED_FROM]->(l:Location) RETURN p.name LIMIT 5");
        REQUIRE_FALSE(q.matches[0].limit.has_value());
    }

    SECTION("a schema-mandatory join preserves every row, so the bound is safe") {
        GqlVirtualCatalog::local().clear();
        GqlVirtualCatalog::local().add_constraint(
            "MandatoryShippedFrom",
            "MATCH (s:Shipment) WHERE NOT EXISTS { MATCH (s)-[:SHIPPED_FROM]->(l:Location) } RETURN s");
        auto q = optimized(
            "MATCH (s:Shipment) MATCH (s:Shipment)-[:SHIPPED_FROM]->(l:Location) RETURN s LIMIT 5");
        REQUIRE(q.matches[0].limit.has_value());
        REQUIRE(q.matches[0].limit.value() == 5);
        GqlVirtualCatalog::local().clear();
    }

    SECTION("the mandatory join must spell the label out, even on an already-bound variable") {
        // Matching the constraint reads the label off the pattern, so re-using a variable bound by an
        // earlier match without repeating its label leaves the join unproven and the scan unbounded.
        // Conservative, so it costs a bounded scan rather than correctness -- pinned because the pass
        // builds a variable-to-label map for exactly this case and then never reads it.
        GqlVirtualCatalog::local().clear();
        GqlVirtualCatalog::local().add_constraint(
            "MandatoryShippedFrom",
            "MATCH (s:Shipment) WHERE NOT EXISTS { MATCH (s)-[:SHIPPED_FROM]->(l:Location) } RETURN s");
        auto q = optimized(
            "MATCH (s:Shipment) MATCH (s)-[:SHIPPED_FROM]->(l:Location) RETURN s LIMIT 5");
        REQUIRE_FALSE(q.matches[0].limit.has_value());
        GqlVirtualCatalog::local().clear();
    }
}

TEST_CASE("limit pushdown does not bound a segment that consumes piped rows", "[gql_optimizer]") {
    GqlVirtualCatalog::local().clear();
    // A continuation segment's LIMIT bounds its total output, not the per-input-row expansion that a
    // per-match scan limit would bound.
    auto q = optimized("MATCH (p:Person) RETURN p AS person NEXT MATCH (f:Person) RETURN f.name LIMIT 5");
    REQUIRE(q.consumes_piped_rows);
    REQUIRE(q.matches.size() == 1);
    REQUIRE_FALSE(q.matches[0].limit.has_value());
}
