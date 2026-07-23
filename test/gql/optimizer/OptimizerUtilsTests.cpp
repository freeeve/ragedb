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

TEST_CASE("is_equivalent_pattern compares nodes and edges position by position", "[gql_optimizer]") {
    auto pat = [](const std::string& p) {
        return GqlParser::parse("MATCH " + p + " RETURN a").matches[0].pattern;
    };
    REQUIRE(is_equivalent_pattern(pat("(a:P)-[:R]->(b:P)"), pat("(a:P)-[:R]->(b:P)")));
    // Different target variable, same shape -- not equivalent (subsumption handles that case separately).
    REQUIRE_FALSE(is_equivalent_pattern(pat("(a:P)-[:R]->(b:P)"), pat("(a:P)-[:R]->(c:P)")));
    // Different relationship type.
    REQUIRE_FALSE(is_equivalent_pattern(pat("(a:P)-[:R]->(b:P)"), pat("(a:P)-[:S]->(b:P)")));
    // Different edge direction.
    REQUIRE_FALSE(is_equivalent_pattern(pat("(a:P)-[:R]->(b:P)"), pat("(a:P)<-[:R]-(b:P)")));
    // Different node count.
    REQUIRE_FALSE(is_equivalent_pattern(pat("(a:P)"), pat("(a:P)-[:R]->(b:P)")));
}

TEST_CASE("is_label_subsumed treats an absent label as a wildcard", "[gql_optimizer]") {
    auto lbl = [](const std::string& node) {
        return GqlParser::parse("MATCH " + node + " RETURN a").matches[0].pattern.nodes[0].label_expr;
    };
    auto labeled = lbl("(a:P)");
    auto unlabeled = lbl("(a)");
    // A specific label is subsumed by an absent (wildcard) one.
    REQUIRE(is_label_subsumed(labeled, unlabeled));
    // A wildcard is NOT subsumed by a specific label.
    REQUIRE_FALSE(is_label_subsumed(unlabeled, labeled));
    // Same label subsumes; a different one does not.
    REQUIRE(is_label_subsumed(labeled, lbl("(a:P)")));
    REQUIRE_FALSE(is_label_subsumed(labeled, lbl("(a:Q)")));
}

TEST_CASE("extract_filters pushes conjuncts but never descends into an OR", "[gql_optimizer]") {
    auto where = [](const std::string& w) {
        return GqlParser::parse("MATCH (a:Person) WHERE " + w + " RETURN a").where_expr;
    };
    SECTION("an AND of literal comparisons pushes both") {
        std::map<std::string, std::vector<PropertyFilter>> pd;
        auto w = where("a.x = 1 AND a.y > 2");
        extract_filters(w.get(), pd);
        REQUIRE(pd["a"].size() == 2);
    }
    SECTION("an OR pushes nothing (unsound to push a disjunct as a scan filter)") {
        std::map<std::string, std::vector<PropertyFilter>> pd;
        auto w = where("a.x = 1 OR a.y = 2");
        extract_filters(w.get(), pd);
        REQUIRE(pd.empty());
    }
}

TEST_CASE("collect_variables_from_matches gathers node and edge variables", "[gql_optimizer]") {
    auto q = GqlParser::parse("MATCH (a:P)-[e:R]->(b:P) RETURN a");
    std::set<std::string> vars;
    collect_variables_from_matches(q.matches, vars);
    REQUIRE(vars.count("a") == 1);
    REQUIRE(vars.count("b") == 1);
    REQUIRE(vars.count("e") == 1);
}

TEST_CASE("query_has_distinct_aggregate detects count(DISTINCT ...)", "[gql_optimizer]") {
    REQUIRE(query_has_distinct_aggregate(GqlParser::parse("MATCH (a:P) RETURN count(DISTINCT a) AS n")));
    REQUIRE_FALSE(query_has_distinct_aggregate(GqlParser::parse("MATCH (a:P) RETURN count(a) AS n")));
}

namespace {
// Extract the range intervals a WHERE predicate implies for variable x. This is the analysis that lets the
// contradiction/domain pruners decide a query is unsatisfiable (an empty interval), so its operator mapping,
// operand reversal, and NOT/De-Morgan handling all have to be exact.
std::map<std::string, Interval> intervals_for(const std::string& where) {
    auto q = GqlParser::parse("MATCH (x) WHERE " + where + " RETURN x");
    std::map<std::string, Interval> ivs;
    extract_intervals_from_expr(q.where_expr.get(), "x", ivs, false);
    return ivs;
}
}  // namespace

TEST_CASE("extract_intervals_from_expr maps each comparison operator to a bound", "[gql_optimizer]") {
    SECTION("x.age < 5 is an exclusive upper bound") {
        auto iv = intervals_for("x.age < 5").at("age");
        REQUIRE(iv.has_upper); REQUIRE(iv.upper_val == 5); REQUIRE_FALSE(iv.upper_inclusive);
        REQUIRE_FALSE(iv.has_lower);
    }
    SECTION("x.age <= 5 is an inclusive upper bound") {
        auto iv = intervals_for("x.age <= 5").at("age");
        REQUIRE(iv.has_upper); REQUIRE(iv.upper_val == 5); REQUIRE(iv.upper_inclusive);
    }
    SECTION("x.age > 5 is an exclusive lower bound") {
        auto iv = intervals_for("x.age > 5").at("age");
        REQUIRE(iv.has_lower); REQUIRE(iv.lower_val == 5); REQUIRE_FALSE(iv.lower_inclusive);
        REQUIRE_FALSE(iv.has_upper);
    }
    SECTION("x.age >= 5 is an inclusive lower bound") {
        auto iv = intervals_for("x.age >= 5").at("age");
        REQUIRE(iv.has_lower); REQUIRE(iv.lower_val == 5); REQUIRE(iv.lower_inclusive);
    }
    SECTION("x.age = 5 pins both bounds to the same point") {
        auto iv = intervals_for("x.age = 5").at("age");
        REQUIRE(iv.has_lower); REQUIRE(iv.lower_val == 5); REQUIRE(iv.lower_inclusive);
        REQUIRE(iv.has_upper); REQUIRE(iv.upper_val == 5); REQUIRE(iv.upper_inclusive);
        REQUIRE_FALSE(iv.is_empty());
    }
}

TEST_CASE("extract_intervals_from_expr normalizes a comparison with the constant on the left", "[gql_optimizer]") {
    SECTION("5 < x.age is read as x.age > 5") {
        auto iv = intervals_for("5 < x.age").at("age");
        REQUIRE(iv.has_lower); REQUIRE(iv.lower_val == 5); REQUIRE_FALSE(iv.lower_inclusive);
    }
    SECTION("5 >= x.age is read as x.age <= 5") {
        auto iv = intervals_for("5 >= x.age").at("age");
        REQUIRE(iv.has_upper); REQUIRE(iv.upper_val == 5); REQUIRE(iv.upper_inclusive);
    }
}

TEST_CASE("extract_intervals_from_expr intersects an AND and detects a contradiction", "[gql_optimizer]") {
    SECTION("x.age >= 3 AND x.age <= 10 is the closed range [3, 10]") {
        auto iv = intervals_for("x.age >= 3 AND x.age <= 10").at("age");
        REQUIRE(iv.lower_val == 3); REQUIRE(iv.upper_val == 10);
        REQUIRE_FALSE(iv.is_empty());
    }
    SECTION("x.age >= 10 AND x.age <= 3 is an empty interval -- the pruner's unsatisfiable case") {
        auto iv = intervals_for("x.age >= 10 AND x.age <= 3").at("age");
        REQUIRE(iv.is_empty());
    }
}

TEST_CASE("extract_intervals_from_expr applies NOT and De Morgan", "[gql_optimizer]") {
    SECTION("NOT (x.age < 5) flips to x.age >= 5") {
        auto iv = intervals_for("NOT (x.age < 5)").at("age");
        REQUIRE(iv.has_lower); REQUIRE(iv.lower_val == 5); REQUIRE(iv.lower_inclusive);
    }
    SECTION("NOT (x.age > 3 OR x.age > 10) becomes x.age <= 3 (the tighter bound)") {
        auto iv = intervals_for("NOT (x.age > 3 OR x.age > 10)").at("age");
        REQUIRE(iv.has_upper); REQUIRE(iv.upper_val == 3); REQUIRE(iv.upper_inclusive);
        REQUIRE_FALSE(iv.has_lower);
    }
    SECTION("NOT (x.age = 5) yields no interval -- inequality is not a single range") {
        REQUIRE(intervals_for("NOT (x.age = 5)").empty());
    }
    SECTION("NOT (x.age > 3 AND x.age < 10) yields no interval -- a negated conjunction is a disjunction") {
        REQUIRE(intervals_for("NOT (x.age > 3 AND x.age < 10)").empty());
    }
}

TEST_CASE("extract_intervals_from_expr ignores predicates outside the target variable's scope", "[gql_optimizer]") {
    SECTION("a comparison on a different variable contributes nothing") {
        REQUIRE(intervals_for("y.age > 5").empty());
    }
    SECTION("a non-numeric comparison contributes nothing") {
        REQUIRE(intervals_for("x.name > 'foo'").empty());
    }
}

namespace {
const VarInfo* find_var(const std::vector<VarInfo>& vs, const std::string& name) {
    for (const auto& v : vs) {
        if (v.variable == name) return &v;
    }
    return nullptr;
}
}  // namespace

TEST_CASE("collect_query_vars gathers node variables but excludes edges", "[gql_optimizer]") {
    auto vs = collect_query_vars(GqlParser::parse("MATCH (a:Person)-[e:KNOWS]->(b:Person) RETURN a"));
    REQUIRE(vs.size() == 2);
    REQUIRE(find_var(vs, "a") != nullptr);
    REQUIRE(find_var(vs, "b") != nullptr);
    REQUIRE(find_var(vs, "e") == nullptr);          // the edge variable is a node-only pass concern
    REQUIRE(find_var(vs, "a")->label == "Person");
}

TEST_CASE("collect_all_query_vars also gathers edge variables", "[gql_optimizer]") {
    auto vs = collect_all_query_vars(GqlParser::parse("MATCH (a:Person)-[e:KNOWS]->(b:Person) RETURN a"));
    REQUIRE(vs.size() == 3);
    const VarInfo* e = find_var(vs, "e");
    REQUIRE(e != nullptr);
    REQUIRE(e->label == "KNOWS");
}

TEST_CASE("collect_query_vars skips anonymous nodes and edges", "[gql_optimizer]") {
    auto vs = collect_query_vars(GqlParser::parse("MATCH (a:Person)-[:KNOWS]->() RETURN a"));
    REQUIRE(vs.size() == 1);
    REQUIRE(vs[0].variable == "a");
}

TEST_CASE("collect_query_vars records a label only for a plain literal label", "[gql_optimizer]") {
    SECTION("a single label is captured") {
        auto vs = collect_query_vars(GqlParser::parse("MATCH (a:Person) RETURN a"));
        REQUIRE(find_var(vs, "a")->label == "Person");
    }
    SECTION("a composite label expression leaves the label empty (subsumption handles it elsewhere)") {
        auto vs = collect_query_vars(GqlParser::parse("MATCH (a:Person&Employee) RETURN a"));
        REQUIRE(find_var(vs, "a")->label.empty());
    }
}

TEST_CASE("collect_query_vars folds property constraints into a variable's intervals", "[gql_optimizer]") {
    auto interval_of = [](const std::string& q, const std::string& prop) {
        return collect_query_vars(GqlParser::parse(q)).at(0).intervals.at(prop);
    };
    SECTION("an inline map property is an equality point") {
        auto iv = interval_of("MATCH (a:Person {age: 30}) RETURN a", "age");
        REQUIRE(iv.has_lower); REQUIRE(iv.lower_val == 30); REQUIRE(iv.lower_inclusive);
        REQUIRE(iv.has_upper); REQUIRE(iv.upper_val == 30); REQUIRE(iv.upper_inclusive);
    }
    SECTION("a segment WHERE comparison becomes a bound") {
        auto iv = interval_of("MATCH (a:Person) WHERE a.age > 30 RETURN a", "age");
        REQUIRE(iv.has_lower); REQUIRE(iv.lower_val == 30); REQUIRE_FALSE(iv.lower_inclusive);
    }
    SECTION("an inline node WHERE comparison becomes the same bound") {
        auto iv = interval_of("MATCH (a:Person WHERE a.age > 30) RETURN a", "age");
        REQUIRE(iv.has_lower); REQUIRE(iv.lower_val == 30); REQUIRE_FALSE(iv.lower_inclusive);
    }
}

TEST_CASE("collect_all_query_vars folds an edge's map property into its intervals", "[gql_optimizer]") {
    auto vs = collect_all_query_vars(GqlParser::parse("MATCH (a)-[e:KNOWS {weight: 5}]->(b) RETURN a"));
    const VarInfo* e = find_var(vs, "e");
    REQUIRE(e != nullptr);
    auto iv = e->intervals.at("weight");
    REQUIRE(iv.has_lower); REQUIRE(iv.lower_val == 5); REQUIRE(iv.lower_inclusive);
    REQUIRE(iv.has_upper); REQUIRE(iv.upper_val == 5); REQUIRE(iv.upper_inclusive);
}
