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

// The shared optimizer helpers are pure functions the passes lean on for their correctness decisions:
// Interval arithmetic decides whether a range predicate is contradictory (an empty interval prunes the
// query), and has_post_scan_residual_predicate is the exact gate that stops limit pushdown from
// truncating a scan whose rows a later predicate still filters. Both are AST/value level, so they are
// pinned directly rather than through a query result.

#include <catch2/catch.hpp>
#include "../../../src/gql/GqlParser.h"
#include "../../../src/gql/optimizer/OptimizerUtils.h"

using namespace ragedb;
using namespace ragedb::gql;

namespace {
Interval closed(double lo, double hi) {
    Interval iv;
    iv.has_lower = true; iv.lower_val = lo; iv.lower_inclusive = true;
    iv.has_upper = true; iv.upper_val = hi; iv.upper_inclusive = true;
    return iv;
}
}  // namespace

TEST_CASE("Interval::is_empty detects contradictory bounds", "[gql_optimizer]") {
    SECTION("a normal range is not empty") {
        REQUIRE_FALSE(closed(1, 5).is_empty());
    }
    SECTION("lower above upper is empty") {
        Interval iv = closed(5, 1);
        REQUIRE(iv.is_empty());
    }
    SECTION("a single point is non-empty only when both ends include it") {
        REQUIRE_FALSE(closed(3, 3).is_empty());
        Interval half;                 // [3, 3) -- includes lower, excludes upper
        half.has_lower = true; half.lower_val = 3; half.lower_inclusive = true;
        half.has_upper = true; half.upper_val = 3; half.upper_inclusive = false;
        REQUIRE(half.is_empty());
    }
    SECTION("a half-open interval (one bound only) is never empty") {
        Interval lower_only;
        lower_only.has_lower = true; lower_only.lower_val = 10; lower_only.lower_inclusive = true;
        REQUIRE_FALSE(lower_only.is_empty());
    }
}

TEST_CASE("Interval::intersect narrows to the tighter of each bound", "[gql_optimizer]") {
    SECTION("overlapping ranges intersect to their overlap") {
        Interval a = closed(1, 10);
        a.intersect(closed(5, 20));
        REQUIRE(a.has_lower); REQUIRE(a.lower_val == 5);
        REQUIRE(a.has_upper); REQUIRE(a.upper_val == 10);
        REQUIRE_FALSE(a.is_empty());
    }
    SECTION("disjoint ranges intersect to an empty interval (x > 5 AND x < 3)") {
        Interval a;                    // x > 5
        a.has_lower = true; a.lower_val = 5; a.lower_inclusive = false;
        Interval b;                    // x < 3
        b.has_upper = true; b.upper_val = 3; b.upper_inclusive = false;
        a.intersect(b);
        REQUIRE(a.is_empty());
    }
    SECTION("intersecting an unbounded interval adopts the other's bounds") {
        Interval a;                    // unbounded
        a.intersect(closed(2, 8));
        REQUIRE(a.has_lower); REQUIRE(a.lower_val == 2);
        REQUIRE(a.has_upper); REQUIRE(a.upper_val == 8);
    }
    SECTION("the tighter exclusivity wins at an equal bound") {
        Interval a = closed(0, 10);    // upper inclusive at 10
        Interval b;                    // x < 10 (exclusive)
        b.has_upper = true; b.upper_val = 10; b.upper_inclusive = false;
        a.intersect(b);
        REQUIRE(a.has_upper); REQUIRE(a.upper_val == 10); REQUIRE_FALSE(a.upper_inclusive);
    }
}

TEST_CASE("Interval::contains respects boundary inclusivity", "[gql_optimizer]") {
    REQUIRE(closed(0, 10).contains(closed(2, 8)));
    REQUIRE_FALSE(closed(2, 8).contains(closed(0, 10)));
    // A closed bound contains an open one at the same value, but not vice versa.
    Interval open_lo;
    open_lo.has_lower = true; open_lo.lower_val = 0; open_lo.lower_inclusive = false;
    open_lo.has_upper = true; open_lo.upper_val = 10; open_lo.upper_inclusive = true;
    REQUIRE(closed(0, 10).contains(open_lo));
    REQUIRE_FALSE(open_lo.contains(closed(0, 10)));
}

TEST_CASE("get_numeric_value reads literals and negation", "[gql_optimizer]") {
    auto num = [](const std::string& expr) {
        // Parse a trivial query and pull the RETURN item's expression.
        auto q = GqlParser::parse("MATCH (n:N) RETURN " + expr + " AS v");
        double out = -999;
        bool ok = get_numeric_value(q.returns[0].expr.get(), out);
        return std::make_pair(ok, out);
    };
    REQUIRE(num("42").first);
    REQUIRE(num("42").second == 42.0);
    REQUIRE(num("-7").second == -7.0);
    REQUIRE(num("3.5").second == 3.5);
    // A property lookup is not a numeric constant.
    REQUIRE_FALSE(num("n.age").first);
}

TEST_CASE("has_post_scan_residual_predicate flags rows a later filter still drops", "[gql_optimizer]") {
    auto residual = [](const std::string& q) {
        return has_post_scan_residual_predicate(GqlParser::parse(q));
    };
    SECTION("a bare labeled scan has no residual") {
        REQUIRE_FALSE(residual("MATCH (p:Person) RETURN p"));
    }
    SECTION("a segment WHERE is a residual predicate") {
        REQUIRE(residual("MATCH (p:Person) WHERE p.age > 30 RETURN p"));
    }
    SECTION("DISTINCT collapses rows after the scan") {
        REQUIRE(residual("MATCH (p:Person) RETURN DISTINCT p.name"));
    }
    SECTION("an inline node WHERE is a residual predicate") {
        REQUIRE(residual("MATCH (p:Person WHERE p.age > 30) RETURN p"));
    }
    SECTION("a property filter on a non-anchor node post-filters") {
        REQUIRE(residual("MATCH (p:Person)-[:KNOWS]->(f:Person {name: 'Bob'}) RETURN p"));
    }
}
