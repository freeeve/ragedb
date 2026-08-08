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

// EqualityJoinEliminator collapses two isomorphic self-join arms whose targets are equated in the
// WHERE (MATCH (a)-[:R]->(b), (a)-[:R]->(c) WHERE b = c). Unlike a bare mandatory-join strip, the
// equality pins b and c to the same node, so the cross product collapses to its diagonal -- exactly one
// binding per R-neighbour, multiplicity 1. Erasing the duplicate arm therefore preserves the result
// under every semantics (bag, count, DISTINCT). The pass had only a timing benchmark; these pin that it
// collapses when the equality makes it sound and leaves a plain self-join alone when it does not.

#include <catch2/catch.hpp>
#include <set>
#include <string>
#include "../../../src/gql/GqlParser.h"
#include "../../../src/gql/GqlOptimizer.h"
#include "../../../src/gql/GqlVirtualCatalog.h"
#include "../../../src/gql/optimizer/OptimizerUtils.h"

using namespace ragedb;
using namespace ragedb::gql;

namespace {
GqlQuery optimized(const std::string& text) {
    GqlVirtualCatalog::local().clear();
    auto query = GqlParser::parse(text);
    GqlOptimizer::optimize(query);
    return query;
}
}  // namespace

TEST_CASE("equality join elimination collapses an equated self-join", "[gql_optimizer]") {
    SECTION("bag projection: WHERE b = c collapses the two arms to one") {
        auto q = optimized("MATCH (a:Person)-[:KNOWS]->(b:Person) MATCH (a:Person)-[:KNOWS]->(c:Person) "
                           "WHERE b = c RETURN a.name");
        REQUIRE(q.matches.size() == 1);
    }
    SECTION("count(*): the collapse is still sound (diagonal = single traversal)") {
        auto q = optimized("MATCH (a:Person)-[:KNOWS]->(b:Person) MATCH (a:Person)-[:KNOWS]->(c:Person) "
                           "WHERE b = c RETURN count(*) AS n");
        REQUIRE(q.matches.size() == 1);
    }
    SECTION("DISTINCT: collapses as well") {
        auto q = optimized("MATCH (a:Person)-[:KNOWS]->(b:Person) MATCH (a:Person)-[:KNOWS]->(c:Person) "
                           "WHERE b = c RETURN DISTINCT a.name");
        REQUIRE(q.matches.size() == 1);
    }
}

TEST_CASE("equality expressed through id properties collapses the same join", "[gql_optimizer]") {
    // `b.id = c.id` and `b = c` pin the same two variables to the same node, so the pass has to treat
    // them alike. Only the bare-variable spelling was covered, leaving the id form -- both recognising it
    // and removing it afterwards -- unexercised.
    SECTION("b.id = c.id collapses the arms") {
        auto q = optimized("MATCH (a:Person)-[:KNOWS]->(b:Person) MATCH (a:Person)-[:KNOWS]->(c:Person) "
                           "WHERE b.id = c.id RETURN a.name");
        REQUIRE(q.matches.size() == 1);
    }

    SECTION("a property other than id does not equate the variables") {
        // Two people can share a name without being the same node, so this is a real filter, not an
        // identity, and the arms must stay.
        auto q = optimized("MATCH (a:Person)-[:KNOWS]->(b:Person) MATCH (a:Person)-[:KNOWS]->(c:Person) "
                           "WHERE b.name = c.name RETURN a.name");
        REQUIRE(q.matches.size() == 2);
    }

    SECTION("the equated pair is recognised whichever order it is written") {
        // The pair is normalised before lookup, so naming the later variable first must still match.
        auto q = optimized("MATCH (a:Person)-[:KNOWS]->(b:Person) MATCH (a:Person)-[:KNOWS]->(c:Person) "
                           "WHERE c = b RETURN a.name");
        REQUIRE(q.matches.size() == 1);
    }

    SECTION("unrelated conjuncts either side of the equality are kept") {
        // Removing the equality must rebuild the remaining AND rather than drop the siblings with it.
        // The siblings are property-to-property comparisons on purpose: a literal comparison would be
        // lifted into the scan by pushdown and correctly vanish from the residual, which would test the
        // wrong thing.
        auto q = optimized("MATCH (a:Person)-[:KNOWS]->(b:Person) MATCH (a:Person)-[:KNOWS]->(c:Person) "
                           "WHERE a.x = a.y AND b = c AND a.p = a.q RETURN a.name");
        REQUIRE(q.matches.size() == 1);
        REQUIRE(q.where_expr != nullptr);
        REQUIRE(q.where_expr->kind == ExpressionKind::BINARY_OP);
        REQUIRE(static_cast<const BinaryOpExpr*>(q.where_expr.get())->op == BinaryOpKind::AND);
    }

    SECTION("an equality whose siblings are all pushed leaves no residual at all") {
        // The complement of the case above, recorded so the empty residual reads as intended rather than
        // as something lost: both literal comparisons become scan filters and the equality is removed.
        auto q = optimized("MATCH (a:Person)-[:KNOWS]->(b:Person) MATCH (a:Person)-[:KNOWS]->(c:Person) "
                           "WHERE a.age > 1 AND b = c AND a.rank < 9 RETURN a.name");
        REQUIRE(q.matches.size() == 1);
        REQUIRE(q.where_expr == nullptr);
    }
}

TEST_CASE("equality join elimination never renames a reference into a scoped element's name",
          "[gql_optimizer]") {
    // Merging rewrites references from the pruned variable to the kept one. If a comprehension already
    // uses the kept name for its element, an outer reference rewritten into the body stops meaning the
    // node and starts meaning the list item -- wrong values rather than an error.
    auto q = optimized("MATCH (a:P)-[:R]->(b:P) MATCH (a:P)-[:R]->(c:P) WHERE b = c "
                       "RETURN [c IN a.vals | c + b.age] AS l");

    const auto* lc = static_cast<const ListComprehensionExpr*>(q.returns[0].expr.get());
    REQUIRE(lc != nullptr);

    // Whatever the pass decides, the body's outer reference must still name a variable a surviving
    // pattern binds, and must not have become the comprehension's element.
    const auto* sum = static_cast<const BinaryOpExpr*>(lc->projection.get());
    REQUIRE(sum != nullptr);
    REQUIRE(sum->right->kind == ExpressionKind::PROPERTY_LOOKUP);
    const std::string outer = static_cast<const PropertyLookupExpr*>(sum->right.get())->variable;
    REQUIRE(outer != lc->variable);

    std::set<std::string> bound;
    collect_variables_from_matches(q.matches, bound);
    REQUIRE(bound.count(outer) == 1);
}

TEST_CASE("equality join elimination leaves a genuine self-join in place", "[gql_optimizer]") {
    SECTION("no equality: the two arms are an independent bag self-join, not collapsed") {
        auto q = optimized("MATCH (a:Person)-[:KNOWS]->(b:Person) MATCH (a:Person)-[:KNOWS]->(c:Person) "
                           "RETURN a.name");
        REQUIRE(q.matches.size() == 2);
    }
    SECTION("an inequality does not equate the targets") {
        auto q = optimized("MATCH (a:Person)-[:KNOWS]->(b:Person) MATCH (a:Person)-[:KNOWS]->(c:Person) "
                           "WHERE b <> c RETURN a.name");
        REQUIRE(q.matches.size() == 2);
    }
}
