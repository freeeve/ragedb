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

TEST_CASE("GQL Optimizer Phase 4: Algebraic Query Rewrites (Aggregation Pushdown)", "[gql_optimizer][semantic]") {
    GqlVirtualCatalog::local().clear();

    std::string query_str = "MATCH (p:Person)-[:FRIEND]->(f:Person) RETURN p.name, sum(p.age * f.age)";
    auto query = GqlParser::parse(query_str);
    GqlOptimizer::optimize(query);

    // The sum(p.age * f.age) aggregation should be rewritten to p.age * sum(f.age)
    REQUIRE(query.returns[1].expr->kind == ExpressionKind::BINARY_OP);
    auto* bin = static_cast<const BinaryOpExpr*>(query.returns[1].expr.get());
    REQUIRE(bin->op == BinaryOpKind::MUL);

    // Left should be p.age
    REQUIRE(bin->left->kind == ExpressionKind::PROPERTY_LOOKUP);
    auto* pl = static_cast<const PropertyLookupExpr*>(bin->left.get());
    REQUIRE(pl->variable == "p");
    REQUIRE(pl->property == "age");

    // Right should be sum(f.age)
    REQUIRE(bin->right->kind == ExpressionKind::AGGREGATION);
    auto* agg = static_cast<const AggregateExpr*>(bin->right.get());
    REQUIRE(agg->fn_kind == AggregateKind::SUM);
    REQUIRE(agg->expr->kind == ExpressionKind::PROPERTY_LOOKUP);
    auto* pl_agg = static_cast<const PropertyLookupExpr*>(agg->expr.get());
    REQUIRE(pl_agg->variable == "f");
    REQUIRE(pl_agg->property == "age");
}

TEST_CASE("GQL Optimizer Algebraic Path Count Rewrite", "[gql_optimizer]") {
    GqlVirtualCatalog::local().clear();

    SECTION("Case 1: Variable-length pattern (a)-[:EDGE]->{k}(d)") {
        std::string query_str = "MATCH (a:Person)-[:FRIEND]->{3}(d) RETURN a, count(d)";
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);

        // Assert optimizer flags are set
        const auto& match = query.matches[0];
        REQUIRE(match.algebraic_path_count == true);
        REQUIRE(match.path_count_hops == 3);
        REQUIRE(match.path_count_target_var == "d");
        REQUIRE(match.path_count_rel_types == std::vector<std::string>{"FRIEND"});
        REQUIRE(match.path_count_dir == EdgeDirection::RIGHT);

        // Pattern should be truncated to just node 'a'
        REQUIRE(match.pattern.nodes.size() == 1);
        REQUIRE(match.pattern.nodes[0].variable == "a");
        REQUIRE(match.pattern.edges.empty());

        // Return expression should be rewritten to just 'd'
        REQUIRE(query.returns[1].expr->kind == ExpressionKind::VARIABLE);
        REQUIRE(static_cast<const VariableExpr*>(query.returns[1].expr.get())->name == "d");
        REQUIRE(query.returns[1].alias == "count(d)");
    }

    SECTION("Case 2: Hop chain (a)-[:EDGE]->(b)-[:EDGE]->(c)-[:EDGE]->(d)") {
        std::string query_str = "MATCH (a:Person)-[:FRIEND]->(b)-[:FRIEND]->(c)-[:FRIEND]->(d) RETURN a, count(d)";
        auto query = GqlParser::parse(query_str);
        GqlOptimizer::optimize(query);

        const auto& match = query.matches[0];
        REQUIRE(match.algebraic_path_count == true);
        REQUIRE(match.path_count_hops == 3);
        REQUIRE(match.path_count_target_var == "d");
        REQUIRE(match.path_count_rel_types == std::vector<std::string>{"FRIEND"});
        REQUIRE(match.path_count_dir == EdgeDirection::RIGHT);

        // Pattern should be truncated to just node 'a'
        REQUIRE(match.pattern.nodes.size() == 1);
        REQUIRE(match.pattern.nodes[0].variable == "a");
        REQUIRE(match.pattern.edges.empty());

        // Return expression should be rewritten to just 'd'
        REQUIRE(query.returns[1].expr->kind == ExpressionKind::VARIABLE);
        REQUIRE(static_cast<const VariableExpr*>(query.returns[1].expr.get())->name == "d");
        REQUIRE(query.returns[1].alias == "count(d)");
    }
}

TEST_CASE("the path-count rewrite keeps an intermediate node a predicate still reads",
          "[gql_optimizer]") {
    // Collapsing the chain to a hop count discards the intermediate bindings, which is only sound when
    // nothing outside the count reads them. A predicate the reference check could not see got the
    // intermediate truncated away while the filter on it survived.
    auto rewritten_nodes = [](const std::string& text) {
        GqlVirtualCatalog::local().clear();
        auto query = GqlParser::parse(text);
        GqlOptimizer::optimize(query);
        return query.matches.empty() ? 0u : query.matches[0].pattern.nodes.size();
    };

    const std::string chain = "MATCH (a:P)-[:R]->(m)-[:R]->(b:P) ";

    SECTION("with nothing else reading the intermediate, the chain still collapses") {
        // Without this the guard below could pass by disabling the rewrite entirely.
        REQUIRE(rewritten_nodes(chain + "RETURN count(b) AS n") == 1u);
    }

    SECTION("a predicate on the intermediate blocks the collapse") {
        REQUIRE(rewritten_nodes(chain + "WHERE m.age > 1 RETURN count(b) AS n") == 3u);
        REQUIRE(rewritten_nodes(chain + "WHERE m.age IS NULL RETURN count(b) AS n") == 3u);
        REQUIRE(rewritten_nodes(chain + "WHERE m.age IS NOT NULL RETURN count(b) AS n") == 3u);
        REQUIRE(rewritten_nodes(chain + "WHERE EXISTS { MATCH (m)-[:S]->(z:P) } RETURN count(b) AS n") == 3u);
    }
}
