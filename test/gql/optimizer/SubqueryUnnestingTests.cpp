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

// An EXISTS in the WHERE is turned into an OPTIONAL match appended to the query, so the pattern is
// traversed once alongside the main match rather than re-run per row. The WHERE keeps the EXISTS, which
// then reads the binding the optional match produced.
//
// Nothing pinned this directly, and it is not a self-contained rewrite: it consumes the NOT EXISTS that
// anti-join promotion produces, which is why that promotion does not survive the pipeline. Anyone
// changing pass order needs these to fail loudly rather than discovering the interaction downstream.

#include <catch2/catch.hpp>
#include <string>
#include "../../../src/gql/GqlParser.h"
#include "../../../src/gql/GqlOptimizer.h"
#include "../../../src/gql/GqlVirtualCatalog.h"

using namespace ragedb;
using namespace ragedb::gql;

namespace {
GqlQuery optimized(const std::string& text) {
    GqlVirtualCatalog::local().clear();
    auto query = GqlParser::parse(text);
    GqlOptimizer::optimize(query);
    return query;
}

bool binds(const MatchStatement& m, const std::string& var) {
    for (const auto& n : m.pattern.nodes) {
        if (n.variable == var) return true;
    }
    return false;
}
}  // namespace

TEST_CASE("an EXISTS becomes an optional match over its pattern", "[gql_optimizer]") {
    SECTION("the subquery's own variable is bound by a new optional match") {
        auto q = optimized("MATCH (a:Person) WHERE EXISTS { MATCH (a)-[:KNOWS]->(b:Person) } RETURN a.name");
        REQUIRE(q.matches.size() == 2);
        REQUIRE(q.has_unnested_subquery);
        REQUIRE_FALSE(q.matches[0].is_optional);
        REQUIRE(q.matches[1].is_optional);
        REQUIRE(binds(q.matches[1], "b"));
        // The predicate is kept: it now reads the binding the optional match produced.
        REQUIRE(q.where_expr != nullptr);
    }

    SECTION("a filter written inside the subquery travels onto the unnested pattern") {
        // Otherwise the optional match would traverse every neighbour and the filter would be applied
        // somewhere else, or not at all.
        auto q = optimized("MATCH (a:Person) WHERE EXISTS { MATCH (a)-[:KNOWS]->(b:Person) WHERE b.age > 30 } "
                           "RETURN a.name");
        REQUIRE(q.matches.size() == 2);
        REQUIRE(q.matches[1].is_optional);
        bool found = false;
        for (const auto& n : q.matches[1].pattern.nodes) {
            for (const auto& pf : n.property_filters) {
                if (n.variable == "b" && pf.property == "age") found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("a subquery introducing no new variable is still unnested") {
        // Every variable is already bound outside, so there is no fresh binding to name; the pattern is
        // appended anyway rather than being left for a per-row re-run.
        auto q = optimized("MATCH (a:Person)-[:KNOWS]->(b:Person) WHERE EXISTS { MATCH (a)-[:KNOWS]->(b) } "
                           "RETURN a.name");
        REQUIRE(q.matches.size() == 2);
        REQUIRE(q.has_unnested_subquery);
        REQUIRE(q.matches[1].is_optional);
    }

    SECTION("a query with no subquery is left alone") {
        auto q = optimized("MATCH (a:Person) WHERE a.age > 30 RETURN a.name");
        REQUIRE(q.matches.size() == 1);
        REQUIRE_FALSE(q.has_unnested_subquery);
    }
}
