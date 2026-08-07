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

// ORDER BY names the projection's output aliases, but sort keys are evaluated against pre-projection
// rows, so the parser substitutes each alias with the expression it names. The substitution has to reach
// every syntax a sort key can be spelled in: an arm it skips leaves a bare alias that resolves to nothing
// at sort time, and the ordering the query asked for is silently dropped rather than reported. Split out
// of GqlParserTests.cpp, which owns the rest of the parser's surface.

#include <catch2/catch.hpp>
#include "../../src/gql/GqlParser.h"

using namespace ragedb;
using namespace ragedb::gql;

TEST_CASE("ORDER BY resolves RETURN aliases into sort keys", "[gql_parser]") {
    SECTION("aggregate alias becomes the aggregate expression") {
        auto q = GqlParser::parse(
            "MATCH (t:Tag)<-[:HAS_TAG]-(post) RETURN t.name AS name, count(DISTINCT post) AS cnt "
            "ORDER BY cnt DESC, name ASC LIMIT 10");
        REQUIRE(q.order_by.size() == 2);
        REQUIRE(q.order_by[0].expr->kind == ExpressionKind::AGGREGATION);
        auto* agg = static_cast<const AggregateExpr*>(q.order_by[0].expr.get());
        REQUIRE(agg->fn_kind == AggregateKind::COUNT);
        REQUIRE(agg->distinct);
        REQUIRE(q.order_by[1].expr->kind == ExpressionKind::PROPERTY_LOOKUP);
    }

    SECTION("alias inside an arithmetic sort key is substituted") {
        auto q = GqlParser::parse(
            "MATCH (p:Person) RETURN p.age AS a ORDER BY a + 1 DESC");
        REQUIRE(q.order_by[0].expr->kind == ExpressionKind::BINARY_OP);
        auto* bin = static_cast<const BinaryOpExpr*>(q.order_by[0].expr.get());
        REQUIRE(bin->left->kind == ExpressionKind::PROPERTY_LOOKUP);
    }

    SECTION("alias inside a postfix IS-predicate sort key is substituted, not only IS LABELED") {
        // The substitution must descend into every postfix-IS arm; when it skipped IS NULL / IS DIRECTED /
        // IS SOURCE the alias stayed an unresolved variable and the sort key evaluated as null on every row.
        auto q = GqlParser::parse("MATCH (p:Person) RETURN p.age AS a ORDER BY a IS NULL DESC");
        REQUIRE(q.order_by[0].expr->kind == ExpressionKind::IS_NULL_CHECK);
        const auto* inner = static_cast<const IsNullExpr*>(q.order_by[0].expr.get())->expr.get();
        REQUIRE(inner->kind == ExpressionKind::PROPERTY_LOOKUP);
    }

    SECTION("intermediate NEXT segment ORDER BY resolves that segment's aliases") {
        auto q = GqlParser::parse(
            "MATCH (p:Person)<-[:HAS_CREATOR]-(m) RETURN p, count(m) AS cnt ORDER BY cnt DESC LIMIT 3 "
            "NEXT RETURN p.name");
        REQUIRE(q.with_segments.size() == 1);
        REQUIRE(q.with_segments[0]->order_by.size() == 1);
        REQUIRE(q.with_segments[0]->order_by[0].expr->kind == ExpressionKind::AGGREGATION);
    }

    SECTION("non-alias variables in ORDER BY are untouched") {
        auto q = GqlParser::parse("MATCH (p:Person) RETURN p.name AS n ORDER BY p");
        REQUIRE(q.order_by[0].expr->kind == ExpressionKind::VARIABLE);
    }

    SECTION("an alias aggregated over in ORDER BY is substituted") {
        // ORDER BY may aggregate a projected alias. While aggregate arguments were skipped, the alias
        // stayed a bare variable that resolves to nothing at sort time, silently losing the ordering.
        auto q = GqlParser::parse(
            "MATCH (p:Person) RETURN p.age AS a, count(*) AS c ORDER BY max(a)");
        REQUIRE(q.order_by[0].expr->kind == ExpressionKind::AGGREGATION);
        const auto* arg = static_cast<const AggregateExpr*>(q.order_by[0].expr.get())->expr.get();
        REQUIRE(arg != nullptr);
        REQUIRE(arg->kind == ExpressionKind::PROPERTY_LOOKUP);
    }

    SECTION("an aggregate-valued alias is not spliced into another aggregate") {
        // Substituting count(*) into max(...) would nest one aggregate inside another, so the alias is
        // left in place for the typechecker to report.
        auto q = GqlParser::parse("MATCH (p:Person) RETURN count(*) AS c ORDER BY max(c)");
        REQUIRE(q.order_by[0].expr->kind == ExpressionKind::AGGREGATION);
        const auto* arg = static_cast<const AggregateExpr*>(q.order_by[0].expr.get())->expr.get();
        REQUIRE(arg != nullptr);
        REQUIRE(arg->kind != ExpressionKind::AGGREGATION);
    }
}
