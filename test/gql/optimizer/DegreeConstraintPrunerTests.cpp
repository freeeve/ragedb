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

// A pattern count over a single hop can be answered from a stored degree instead of a traversal, which
// the pass does by swapping the SIZE expression for a lookup of a virtual `_deg_*` property. A degree
// counts edges by type and direction and can see nothing else, so the moment the far node or the edge
// carries any constraint the count would silently ignore it and overcount. The guard that declines those
// shapes is the whole of the pass's soundness, and it is what keeps a label expression inside a counted
// subquery pattern being evaluated by the traversal that understands it -- so both sides are pinned here.

#include <catch2/catch.hpp>
#include <string>
#include "../../../src/gql/GqlParser.h"
#include "../../../src/gql/GqlOptimizer.h"
#include "../../../src/gql/GqlVirtualCatalog.h"
#include "../../../src/gql/optimizer/DegreeConstraintPruner.h"

using namespace ragedb;
using namespace ragedb::gql;

namespace {
// Runs the pass alone and reports whether the counted pattern became a degree lookup.
bool became_degree_lookup(const std::string& text) {
    GqlVirtualCatalog::local().clear();
    auto query = GqlParser::parse(text);
    DegreeConstraintPruner::degree_constraint_pruning_pass(query);
    REQUIRE_FALSE(query.returns.empty());
    const bool rewritten = query.returns[0].expr &&
                           query.returns[0].expr->kind == ExpressionKind::PROPERTY_LOOKUP;
    GqlVirtualCatalog::local().clear();
    return rewritten;
}
}  // namespace

TEST_CASE("an unconstrained single-hop count becomes a degree lookup", "[gql_optimizer]") {
    // Positive control: without this the guards below could pass by never rewriting anything.
    REQUIRE(became_degree_lookup("MATCH (a:Person) RETURN size((a)-[:KNOWS]->()) AS n"));

    SECTION("direction and an absent relationship type still qualify") {
        REQUIRE(became_degree_lookup("MATCH (a:Person) RETURN size((a)<-[:KNOWS]-()) AS n"));
        REQUIRE(became_degree_lookup("MATCH (a:Person) RETURN size((a)-[]->()) AS n"));
    }
}

TEST_CASE("a constrained single-hop count keeps its traversal", "[gql_optimizer]") {
    // Each of these carries something a stored degree cannot see. Rewriting them would count every edge
    // of the type and ignore the constraint, reporting a larger number than the query asked for.
    SECTION("a far-node label") {
        REQUIRE_FALSE(became_degree_lookup("MATCH (a:Person) RETURN size((a)-[:KNOWS]->(:Person)) AS n"));
    }
    SECTION("a far-node label expression, which only the traversal can evaluate") {
        REQUIRE_FALSE(became_degree_lookup("MATCH (a:Person) RETURN size((a)-[:KNOWS]->(:Person&Admin)) AS n"));
        REQUIRE_FALSE(became_degree_lookup("MATCH (a:Person) RETURN size((a)-[:KNOWS]->(:!Bot)) AS n"));
    }
    SECTION("a far-node inline property") {
        REQUIRE_FALSE(became_degree_lookup(
            "MATCH (a:Person) RETURN size((a)-[:KNOWS]->({name: 'Bob'})) AS n"));
    }
    SECTION("an edge property") {
        REQUIRE_FALSE(became_degree_lookup(
            "MATCH (a:Person) RETURN size((a)-[:KNOWS {since: 2020}]->()) AS n"));
    }
    SECTION("a variable-length hop is not a degree at all") {
        REQUIRE_FALSE(became_degree_lookup("MATCH (a:Person) RETURN size((a)-[:KNOWS*1..3]->()) AS n"));
    }
}
