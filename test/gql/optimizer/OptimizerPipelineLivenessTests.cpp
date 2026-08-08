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

// The differential tests run each query optimized and with NO_SEMANTIC and require the rows to match.
// That is the safety half, and on its own it is satisfied by an optimizer that does nothing at all -- if
// no pass fires, the two runs are identical by construction. A pass can therefore keep passing both its
// own unit tests and the differential while being undone by a later phase and never reaching a query.
//
// These are the other half: for each rewrite, a shape whose plan a FULL optimize() must visibly change.
// Calling a pass directly cannot show this, because the interference happens between phases. Assertions
// are on the plan rather than on results, so no graph is needed.

#include <catch2/catch.hpp>
#include <string>
#include "../../../src/gql/GqlParser.h"
#include "../../../src/gql/GqlOptimizer.h"
#include "../../../src/gql/GqlVirtualCatalog.h"

using namespace ragedb;
using namespace ragedb::gql;

namespace {
GqlQuery fully_optimized(const std::string& text) {
    GqlVirtualCatalog::local().clear();
    auto query = GqlParser::parse(text);
    GqlOptimizer::optimize(query);
    return query;
}
}  // namespace

TEST_CASE("each rewrite still reaches a query through the whole pipeline", "[gql_optimizer]") {
    SECTION("a single-hop pattern count becomes a degree lookup") {
        auto q = fully_optimized("MATCH (a:Person) RETURN size((a)-[:KNOWS]->()) AS n");
        REQUIRE_FALSE(q.returns.empty());
        REQUIRE(q.returns[0].expr != nullptr);
        REQUIRE(q.returns[0].expr->kind == ExpressionKind::PROPERTY_LOOKUP);
    }

    SECTION("a counted chain collapses to a hop count") {
        auto q = fully_optimized("MATCH (a:P)-[:R]->(m)-[:R]->(b:P) RETURN count(b) AS n");
        REQUIRE_FALSE(q.matches.empty());
        REQUIRE(q.matches[0].pattern.nodes.size() == 1);
    }

    SECTION("two arms equated in the WHERE collapse to one match") {
        auto q = fully_optimized(
            "MATCH (a:Person)-[:KNOWS]->(b:Person) MATCH (a:Person)-[:KNOWS]->(c:Person) "
            "WHERE b = c RETURN a.name");
        REQUIRE(q.matches.size() == 1);
    }

    SECTION("a LIMIT reaches the scan") {
        auto q = fully_optimized("MATCH (p:Person) RETURN p.name LIMIT 5");
        REQUIRE_FALSE(q.matches.empty());
        REQUIRE(q.matches[0].limit.has_value());
    }
}

TEST_CASE("the liveness assertions fail when the optimizer is switched off", "[gql_optimizer]") {
    // An assertion that holds whether or not the pass runs proves nothing about the pass. Each shape
    // above is repeated here with NO_SEMANTIC, and must NOT show the optimized form -- so if a rewrite is
    // ever made unconditional, or an assertion is weakened into something that was always true, this
    // fails rather than the liveness case silently becoming decorative.

    SECTION("no degree lookup without the optimizer") {
        auto q = fully_optimized("NO_SEMANTIC MATCH (a:Person) RETURN size((a)-[:KNOWS]->()) AS n");
        REQUIRE_FALSE(q.returns.empty());
        REQUIRE(q.returns[0].expr != nullptr);
        REQUIRE(q.returns[0].expr->kind != ExpressionKind::PROPERTY_LOOKUP);
    }

    SECTION("no chain collapse without the optimizer") {
        auto q = fully_optimized("NO_SEMANTIC MATCH (a:P)-[:R]->(m)-[:R]->(b:P) RETURN count(b) AS n");
        REQUIRE_FALSE(q.matches.empty());
        REQUIRE(q.matches[0].pattern.nodes.size() == 3);
    }

    SECTION("no join elimination without the optimizer") {
        auto q = fully_optimized(
            "NO_SEMANTIC MATCH (a:Person)-[:KNOWS]->(b:Person) MATCH (a:Person)-[:KNOWS]->(c:Person) "
            "WHERE b = c RETURN a.name");
        REQUIRE(q.matches.size() == 2);
    }

    SECTION("no limit pushdown without the optimizer") {
        auto q = fully_optimized("NO_SEMANTIC MATCH (p:Person) RETURN p.name LIMIT 5");
        REQUIRE_FALSE(q.matches.empty());
        REQUIRE_FALSE(q.matches[0].limit.has_value());
    }
}

TEST_CASE("the anti-join rewrite does not survive the pipeline today", "[gql_optimizer]") {
    // Recording a known limitation rather than omitting the pass from this file and leaving the gap
    // invisible. The promotion replaces the OPTIONAL match with a NOT EXISTS, and the later subquery
    // unnesting expands that NOT EXISTS straight back into an OPTIONAL match, so the query executes as
    // written. Results stay correct -- the traversal is simply not avoided.
    //
    // If pass ordering changes so the rewrite survives, this assertion fails: read that as the
    // limitation being lifted, and move the shape up into the liveness case above.
    auto q = fully_optimized(
        "MATCH (p:Person) OPTIONAL MATCH (p)-[:FRIEND]->(f:Person) WHERE f IS NULL RETURN p.name");
    bool optional_remains = false;
    for (const auto& m : q.matches) {
        if (m.is_optional) optional_remains = true;
    }
    REQUIRE(optional_remains);
}
