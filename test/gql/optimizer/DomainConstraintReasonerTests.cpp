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

TEST_CASE("GQL Optimizer Phase 7: Composite Attribute Domain Constraint Reasoning", "[gql_optimizer][semantic]") {
    GqlVirtualCatalog::local().clear();
    // Register composite constraint: p.age < 18 OR p.status = 'minor' OR p.is_student = true is impossible
    GqlVirtualCatalog::local().add_constraint(
        "CompositePersonConstraint",
        "MATCH (p:Person) WHERE p.age < 18 OR p.status = 'minor' OR p.is_student = true RETURN p"
    );

    SECTION("Satisfiable query matching consistent attributes") {
        // age >= 18 AND status != 'minor' AND is_student != true is consistent
        std::string query_str = "MATCH (x:Person) WHERE x.age >= 18 AND x.status = 'adult' AND x.is_student = false RETURN x.name";
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);
        REQUIRE_FALSE(query.no_op == true);
    }

    SECTION("Unsatisfiable query matching subset of impossible age range") {
        // x.age < 18 contradicts the constraint guarantee (which implies age >= 18)
        std::string query_str = "MATCH (x:Person) WHERE x.age = 15 RETURN x.name";
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);
        REQUIRE(query.no_op == true);
    }

    SECTION("Unsatisfiable query matching subset of impossible string domain") {
        // x.status = 'minor' contradicts the constraint guarantee (which implies status != 'minor')
        std::string query_str = "MATCH (x:Person) WHERE x.status = 'minor' RETURN x.name";
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);
        REQUIRE(query.no_op == true);
    }

    SECTION("Unsatisfiable query matching subset of impossible boolean domain") {
        // x.is_student = true contradicts the constraint guarantee (which implies is_student != true)
        std::string query_str = "MATCH (x:Person) WHERE x.is_student = true RETURN x.name";
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);
        REQUIRE(query.no_op == true);
    }

    SECTION("Satisfiable query with overlapping but consistent ranges") {
        std::string query_str = "MATCH (x:Person) WHERE x.age > 20 RETURN x.name";
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);
        REQUIRE_FALSE(query.no_op == true);
    }

    GqlVirtualCatalog::local().clear();
}

TEST_CASE("domain reasoning reports unsatisfiable only when the query really is", "[gql_optimizer]") {
    // The pass answers with nothing at all when its solve comes back UNSAT, so an encoding slip -- a
    // predicate the query never mentions forced false, a label or property mixed up between the query and
    // the constraint -- would empty queries with obvious answers and look exactly like "no matches".
    // These pin the shapes a single-predicate constraint must leave alone.
    GqlVirtualCatalog::local().clear();
    GqlVirtualCatalog::local().add_constraint(
        "PositiveAge", "MATCH (p:Person) WHERE p.age < 0 RETURN p");

    auto pruned = [](const std::string& text) {
        auto query = GqlParser::parse(text);
        GqlOptimizer::optimize(query);
        return query.no_op;
    };

    SECTION("ranges that clear, straddle, or sit inside the legal region survive") {
        REQUIRE_FALSE(pruned("MATCH (p:Person) WHERE p.age > 20 RETURN p.name"));
        REQUIRE_FALSE(pruned("MATCH (p:Person) WHERE p.age > -5 RETURN p.name"));
        REQUIRE_FALSE(pruned("MATCH (p:Person) WHERE p.age = 30 RETURN p.name"));
        REQUIRE_FALSE(pruned("MATCH (p:Person) WHERE p.age > 10 AND p.age < 50 RETURN p.name"));
    }

    SECTION("a constraint says nothing about other properties, labels, or an unfiltered query") {
        REQUIRE_FALSE(pruned("MATCH (p:Person) WHERE p.score > 10 RETURN p.name"));
        REQUIRE_FALSE(pruned("MATCH (p:Person) WHERE p.age > 10 AND p.score < 5 RETURN p.name"));
        REQUIRE_FALSE(pruned("MATCH (c:Company) WHERE c.age < -5 RETURN c.name"));
        REQUIRE_FALSE(pruned("MATCH (p:Person) RETURN p.name"));
    }

    SECTION("a disjunction survives while one branch remains legal") {
        REQUIRE_FALSE(pruned("MATCH (p:Person) WHERE p.age < -1 OR p.age > 30 RETURN p.name"));
        REQUIRE_FALSE(pruned("MATCH (p:Person) WHERE NOT (p.age < 0) RETURN p.name"));
    }

    SECTION("genuine contradictions are still reported") {
        REQUIRE(pruned("MATCH (p:Person) WHERE p.age < -5 RETURN p.name"));
        REQUIRE(pruned("MATCH (p:Person) WHERE p.age > 50 AND p.age < 10 RETURN p.name"));
    }

    GqlVirtualCatalog::local().clear();
}
