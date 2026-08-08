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

// Predicate pushdown has two halves that must agree: extract_filters lifts the conjuncts a scan can
// evaluate itself, and rebuild_expression_without_pushed_predicates reconstructs what is left so the
// executor still applies everything that was not pushed. A conjunct dropped from one without being
// accounted for in the other silently widens the result, so they are tested together. Split out of
// OptimizerUtilsAnalysisTests.cpp to sit alongside OptimizerFilters.cpp, which owns both functions.

#include <catch2/catch.hpp>
#include <map>
#include <string>
#include <vector>
#include "../../../src/gql/GqlParser.h"
#include "../../../src/gql/optimizer/OptimizerUtils.h"

using namespace ragedb;
using namespace ragedb::gql;

TEST_CASE("a filter written literal-first pushes with the comparison inverted", "[gql_optimizer]") {
    // `5 < a.age` and `a.age > 5` select the same rows, but the scan filter is stored as
    // property-op-value, so the operator has to be flipped when the literal comes first. Getting that
    // backwards would push a filter that selects the complement, and the residual rebuild has to invert
    // identically or it would drop a conjunct the scan never applied.
    auto pushdowns_of = [](Expression* w) {
        std::map<std::string, std::vector<PropertyFilter>> pd;
        extract_filters(w, pd);
        return pd;
    };

    SECTION("each ordering comparison flips") {
        auto check = [&](const std::string& where, Operation expected) {
            auto q = GqlParser::parse("MATCH (a:P) WHERE " + where + " RETURN a");
            auto pd = pushdowns_of(q.where_expr.get());
            REQUIRE(pd.count("a") == 1);
            REQUIRE(pd["a"].size() == 1);
            REQUIRE(pd["a"][0].property == "age");
            REQUIRE(pd["a"][0].op == expected);
        };
        check("5 < a.age", Operation::GT);
        check("5 <= a.age", Operation::GTE);
        check("5 > a.age", Operation::LT);
        check("5 >= a.age", Operation::LTE);
    }

    SECTION("equality and inequality are symmetric, so they do not flip") {
        auto q = GqlParser::parse("MATCH (a:P) WHERE 5 = a.age AND 6 <> a.rank RETURN a");
        auto pd = pushdowns_of(q.where_expr.get());
        REQUIRE(pd["a"].size() == 2);
        REQUIRE(pd["a"][0].op == Operation::EQ);
        REQUIRE(pd["a"][1].op == Operation::NEQ);
    }

    SECTION("the rebuild drops a literal-first conjunct it pushed") {
        auto q = GqlParser::parse("MATCH (a:P) WHERE 5 < a.age RETURN a");
        auto pd = pushdowns_of(q.where_expr.get());
        auto rebuilt = rebuild_expression_without_pushed_predicates(std::move(q.where_expr), pd);
        REQUIRE(rebuilt == nullptr);
    }

    SECTION("a literal-first conjunct that was not pushed survives the rebuild") {
        // Only the age conjunct is pushed; the property-to-property comparison never is, so it remains.
        auto q = GqlParser::parse("MATCH (a:P) WHERE 5 < a.age AND a.x = a.y RETURN a");
        auto pd = pushdowns_of(q.where_expr.get());
        auto rebuilt = rebuild_expression_without_pushed_predicates(std::move(q.where_expr), pd);
        REQUIRE(rebuilt != nullptr);
        REQUIRE(rebuilt->kind == ExpressionKind::BINARY_OP);
        REQUIRE(static_cast<const BinaryOpExpr*>(rebuilt.get())->op == BinaryOpKind::EQ);
    }

    SECTION("two unpushed conjuncts are rebuilt back into an AND") {
        auto q = GqlParser::parse("MATCH (a:P) WHERE a.x = a.y AND a.p = a.q RETURN a");
        auto pd = pushdowns_of(q.where_expr.get());
        auto rebuilt = rebuild_expression_without_pushed_predicates(std::move(q.where_expr), pd);
        REQUIRE(rebuilt != nullptr);
        REQUIRE(rebuilt->kind == ExpressionKind::BINARY_OP);
        REQUIRE(static_cast<const BinaryOpExpr*>(rebuilt.get())->op == BinaryOpKind::AND);
    }
}

TEST_CASE("rebuild_expression_without_pushed_predicates removes exactly the pushed conjuncts", "[gql_optimizer]") {
    // extract_filters populates the pushdown map from a WHERE; rebuild then drops precisely those conjuncts
    // so a pushed predicate is not re-applied after the scan. The two have to agree on what was pushed.
    auto pushdowns_of = [](Expression* w) {
        std::map<std::string, std::vector<PropertyFilter>> pd;
        extract_filters(w, pd);
        return pd;
    };

    SECTION("every conjunct pushed leaves no residual expression") {
        auto q = GqlParser::parse("MATCH (a:P) WHERE a.x = 1 AND a.y > 2 RETURN a");
        auto pd = pushdowns_of(q.where_expr.get());
        auto rebuilt = rebuild_expression_without_pushed_predicates(std::move(q.where_expr), pd);
        REQUIRE(rebuilt == nullptr);
    }
    SECTION("a conjunct that was not pushed survives") {
        // a.x = a.y is property-to-property, so extract_filters never pushes it; only a.x = 1 is pushed,
        // and the surviving residual is the a.x = a.y comparison.
        auto q = GqlParser::parse("MATCH (a:P) WHERE a.x = 1 AND a.x = a.y RETURN a");
        auto pd = pushdowns_of(q.where_expr.get());
        auto rebuilt = rebuild_expression_without_pushed_predicates(std::move(q.where_expr), pd);
        REQUIRE(rebuilt != nullptr);
        REQUIRE(rebuilt->kind == ExpressionKind::BINARY_OP);
        REQUIRE(static_cast<const BinaryOpExpr*>(rebuilt.get())->op == BinaryOpKind::EQ);
    }
    SECTION("an OR is never pushed, so it is kept whole") {
        auto q = GqlParser::parse("MATCH (a:P) WHERE a.x = 1 OR a.y = 2 RETURN a");
        auto pd = pushdowns_of(q.where_expr.get());
        REQUIRE(pd.empty());
        auto rebuilt = rebuild_expression_without_pushed_predicates(std::move(q.where_expr), pd);
        REQUIRE(rebuilt != nullptr);
        REQUIRE(static_cast<const BinaryOpExpr*>(rebuilt.get())->op == BinaryOpKind::OR);
    }
    SECTION("with no pushdowns nothing is removed") {
        auto q = GqlParser::parse("MATCH (a:P) WHERE a.x = 1 RETURN a");
        std::map<std::string, std::vector<PropertyFilter>> empty;
        auto rebuilt = rebuild_expression_without_pushed_predicates(std::move(q.where_expr), empty);
        REQUIRE(rebuilt != nullptr);
    }
}
