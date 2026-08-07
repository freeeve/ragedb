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

// A uniqueness constraint makes the join to the target at most one row, so the traversal can be dropped
// when nothing else needs the target variable. "Nothing else needs it" is the whole licence, and getting
// that answer wrong erases the match while the projection still reads from it -- which surfaces as a
// missing or null column rather than an error. The pass previously had only a timing benchmark.

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
bool target_still_bound(const std::string& projection) {
    GqlVirtualCatalog::local().clear();
    GqlVirtualCatalog::local().add_constraint(
        "UniqueConstraint",
        "MATCH (s:Person)-[r:UNIQUE_REL]->(t1:TargetNode) MATCH (s)-[:UNIQUE_REL]->(t2:TargetNode) "
        "WHERE t1 != t2 RETURN s");
    auto query = GqlParser::parse(
        "OPTIONAL MATCH (p:Person)-[:UNIQUE_REL]->(t:TargetNode) RETURN " + projection);
    GqlOptimizer::optimize(query);
    std::set<std::string> bound;
    collect_variables_from_matches(query.matches, bound);
    GqlVirtualCatalog::local().clear();
    return bound.count("t") == 1;
}
}  // namespace

TEST_CASE("unique join elimination keeps a target the projection still reads", "[gql_optimizer]") {
    SECTION("a bare property reference") {
        REQUIRE(target_still_bound("t.name"));
    }

    SECTION("references wrapped in other syntax") {
        // Each of these once read as "target unused", so whether the value survived came down to how the
        // query happened to spell it.
        REQUIRE(target_still_bound("upper(t.name)"));
        REQUIRE(target_still_bound("CASE WHEN t.name = 'x' THEN 1 ELSE 0 END"));
        REQUIRE(target_still_bound("CAST(t.code AS INTEGER)"));
        REQUIRE(target_still_bound("[t.name]"));
        REQUIRE(target_still_bound("t IS LABELED TargetNode"));
    }
}

TEST_CASE("unique join elimination still drops a target nothing reads", "[gql_optimizer]") {
    // Without this the reference check could pass by always claiming the target is needed.
    REQUIRE_FALSE(target_still_bound("p.name"));
}
