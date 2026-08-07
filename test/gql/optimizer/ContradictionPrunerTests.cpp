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
#include "../../../src/gql/GqlParser.h"
#include "../../../src/gql/GqlOptimizer.h"
#include "../../../src/gql/GqlVirtualCatalog.h"

using namespace ragedb;
using namespace ragedb::gql;

TEST_CASE("GQL Optimizer Phase 1: Primitive Constraints & Contradiction Pruning", "[gql_optimizer][semantic]") {
    GqlVirtualCatalog::local().clear();
    // Register constraint: no Person has age < 0 (i.e. Person.age < 0 is impossible)
    GqlVirtualCatalog::local().add_constraint("PositiveAge", "MATCH (p:Person) WHERE p.age < 0 RETURN p");

    SECTION("Contradiction with constraint bounds") {
        std::string query_str = "MATCH (x:Person) WHERE x.age = -5 RETURN x";
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);
        REQUIRE(query.no_op == true);
    }

    SECTION("Contradiction with constraint range bounds") {
        std::string query_str = "MATCH (x:Person) WHERE x.age < -2 RETURN x";
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);
        REQUIRE(query.no_op == true);
    }

    SECTION("Self contradiction within query filters") {
        std::string query_str = "MATCH (x:Person) WHERE x.age > 10 AND x.age < 5 RETURN x";
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);
        REQUIRE(query.no_op == true);
    }

    GqlVirtualCatalog::local().clear();
}

TEST_CASE("a multi-property constraint forbids only the combination of its bounds", "[gql_optimizer]") {
    GqlVirtualCatalog::local().clear();
    // No Person is BOTH under 18 AND under 10 points. Neither bound is forbidden on its own.
    GqlVirtualCatalog::local().add_constraint(
        "JuniorLowScore", "MATCH (p:Person) WHERE p.age < 18 AND p.score < 10 RETURN p");

    auto pruned = [](const std::string& text) {
        auto query = GqlParser::parse(text);
        GqlOptimizer::optimize(query);
        return query.no_op;
    };

    SECTION("a query bounded by one conjunct alone still has answers") {
        // A ten-year-old with 50 points satisfies age < 15 and violates nothing.
        REQUIRE_FALSE(pruned("MATCH (p:Person) WHERE p.age < 15 RETURN p.name"));
        REQUIRE_FALSE(pruned("MATCH (p:Person) WHERE p.score < 5 RETURN p.name"));
    }

    SECTION("a query implying every conjunct is unsatisfiable") {
        REQUIRE(pruned("MATCH (p:Person) WHERE p.age < 15 AND p.score < 5 RETURN p.name"));
    }

    GqlVirtualCatalog::local().clear();
}

TEST_CASE("GQL Optimizer Phase 3: Multi-Variable Relational Predicate Reasoning", "[gql_optimizer][semantic]") {
    GqlVirtualCatalog::local().clear();

    SECTION("Impossible cycle (strict inequality contradiction)") {
        // a < b AND b <= c AND c < a is impossible (transitivity implies a < a)
        std::string query_str = "MATCH (a:Person), (b:Person), (c:Person) WHERE a.age < b.age AND b.age <= c.age AND c.age < a.age RETURN a";
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);
        REQUIRE(query.no_op == true);
    }

    SECTION("Possible cycle (non-strict cycle)") {
        // a <= b AND b <= c AND c <= a is possible (implies equality)
        std::string query_str = "MATCH (a:Person), (b:Person), (c:Person) WHERE a.age <= b.age AND b.age <= c.age AND c.age <= a.age RETURN a";
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);
        REQUIRE_FALSE(query.no_op == true);
    }
}
