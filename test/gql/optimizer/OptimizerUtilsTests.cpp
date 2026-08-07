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
#include <map>
#include <set>
#include <string>
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

namespace {
// Collects referenced variable names by an independent walk, so a gap in the production walk cannot
// hide behind the same gap here.
void collect_names(const Expression* e, std::set<std::string>& out) {
    if (!e) return;
    switch (e->kind) {
        case ExpressionKind::VARIABLE:
            out.insert(static_cast<const VariableExpr*>(e)->name); return;
        case ExpressionKind::PROPERTY_LOOKUP:
            out.insert(static_cast<const PropertyLookupExpr*>(e)->variable); return;
        case ExpressionKind::UNARY_OP:
            collect_names(static_cast<const UnaryOpExpr*>(e)->expr.get(), out); return;
        case ExpressionKind::BINARY_OP: {
            auto* b = static_cast<const BinaryOpExpr*>(e);
            collect_names(b->left.get(), out); collect_names(b->right.get(), out); return;
        }
        case ExpressionKind::AGGREGATION:
            collect_names(static_cast<const AggregateExpr*>(e)->expr.get(), out); return;
        case ExpressionKind::IS_NULL_CHECK:
            collect_names(static_cast<const IsNullExpr*>(e)->expr.get(), out); return;
        case ExpressionKind::IS_LABELED:
            collect_names(static_cast<const IsLabeledExpr*>(e)->value.get(), out); return;
        case ExpressionKind::IS_DIRECTED:
            collect_names(static_cast<const IsDirectedExpr*>(e)->value.get(), out); return;
        case ExpressionKind::IS_SOURCE_DEST: {
            auto* sd = static_cast<const IsSourceDestExpr*>(e);
            collect_names(sd->value.get(), out); collect_names(sd->edge.get(), out); return;
        }
        case ExpressionKind::CAST:
            collect_names(static_cast<const CastExpr*>(e)->value.get(), out); return;
        case ExpressionKind::FUNCTION_CALL:
            for (const auto& a : static_cast<const FunctionCallExpr*>(e)->args) collect_names(a.get(), out);
            return;
        case ExpressionKind::CASE_WHEN: {
            auto* c = static_cast<const CaseExpr*>(e);
            for (const auto& br : c->branches) {
                collect_names(br.first.get(), out); collect_names(br.second.get(), out);
            }
            collect_names(c->else_expr.get(), out); return;
        }
        case ExpressionKind::IN_LIST: {
            auto* i = static_cast<const InExpr*>(e);
            collect_names(i->value.get(), out); collect_names(i->list.get(), out); return;
        }
        case ExpressionKind::LIST_LITERAL:
            for (const auto& v : static_cast<const ListExpr*>(e)->elements) collect_names(v.get(), out);
            return;
        case ExpressionKind::LIST_INDEX: {
            auto* ix = static_cast<const IndexExpr*>(e);
            collect_names(ix->list.get(), out); collect_names(ix->index.get(), out); return;
        }
        default:
            return;
    }
}

// Renames b -> c inside the projected expression and reports which names survive.
std::set<std::string> renamed_names(const std::string& projection) {
    auto q = GqlParser::parse("MATCH (b:P) RETURN " + projection);
    std::map<std::string, std::string> var_map{{"b", "c"}};
    rewrite_expr_vars(q.returns[0].expr, var_map);
    std::set<std::string> names;
    collect_names(q.returns[0].expr.get(), names);
    return names;
}
}  // namespace

TEST_CASE("query_binds_scoped_variable finds an element name a rename could capture", "[gql_optimizer]") {
    auto binds = [](const std::string& text, const std::string& name) {
        return query_binds_scoped_variable(GqlParser::parse(text), name);
    };
    SECTION("a comprehension element is reported, in the projection or a sort key") {
        REQUIRE(binds("MATCH (n:P) RETURN [c IN n.vals | c] AS l", "c"));
        REQUIRE(binds("MATCH (n:P) RETURN n.name AS x ORDER BY [c IN n.vals | c]", "c"));
    }
    SECTION("a quantified predicate's variable is reported") {
        REQUIRE(binds("MATCH (n:P) WHERE any(c IN n.vals WHERE c > 1) RETURN n.name", "c"));
    }
    SECTION("an unrelated name is not reported") {
        REQUIRE_FALSE(binds("MATCH (n:P) RETURN [c IN n.vals | c] AS l", "b"));
    }
    SECTION("an ordinary pattern variable is not a scoped binder") {
        REQUIRE_FALSE(binds("MATCH (a:P)-[:R]->(c:P) RETURN c.name", "c"));
    }
}

TEST_CASE("expression_references_variable sees a variable through any wrapper", "[gql_optimizer]") {
    // Passes ask this before erasing the match that binds a variable, so a wrapper the walk cannot enter
    // reads as "unused" and takes the binding with it.
    auto refs = [](const std::string& projection, const std::string& name) {
        auto q = GqlParser::parse("MATCH (l:L) RETURN " + projection);
        return expression_references_variable(q.returns[0].expr.get(), name);
    };

    SECTION("through each wrapper that owns a child") {
        REQUIRE(refs("l.name", "l"));
        REQUIRE(refs("upper(l.name)", "l"));
        REQUIRE(refs("CASE WHEN l.name = 'x' THEN 1 ELSE 0 END", "l"));
        REQUIRE(refs("CAST(l.age AS FLOAT)", "l"));
        REQUIRE(refs("[l.name]", "l"));
        REQUIRE(refs("l.name IN ['a']", "l"));
        REQUIRE(refs("l.name IS NULL", "l"));
        REQUIRE(refs("l.created.year", "l"));
    }

    SECTION("and does not invent one") {
        REQUIRE_FALSE(refs("upper(l.name)", "zz"));
    }
}

TEST_CASE("rewrite_expr_vars renames through every expression arm", "[gql_optimizer]") {
    // Callers merge two variables and then erase the match that bound the discarded one, so an arm this
    // walk skips leaves a reference to a variable nothing binds.
    auto renames = [](const std::string& projection) {
        auto names = renamed_names(projection);
        return names.count("b") == 0 && names.count("c") == 1;
    };

    SECTION("arms that already worked") {
        REQUIRE(renames("b.name"));
        REQUIRE(renames("count(b)"));
        REQUIRE(renames("-b.age"));
        REQUIRE(renames("b.age + 1"));
    }
    SECTION("arms that silently skipped the rename") {
        REQUIRE(renames("upper(b.name)"));
        REQUIRE(renames("CASE WHEN b.age > 1 THEN 'y' ELSE 'n' END"));
        REQUIRE(renames("CAST(b.age AS FLOAT)"));
        REQUIRE(renames("b IS LABELED P"));
        REQUIRE(renames("b.age IS NULL"));
        REQUIRE(renames("b.age IN [1, 2]"));
        REQUIRE(renames("[b.name]"));
        REQUIRE(renames("[b.name][0]"));
        // A temporal accessor owns its operand, and being last in the ExpressionKind enum it was the
        // arm every one of these walks originally omitted.
        REQUIRE(renames("b.created.year"));
    }
    SECTION("a scoped iteration variable shadows the rename in the body") {
        // `b` here is the comprehension's element, not the outer node, so the body must be left alone.
        auto q = GqlParser::parse("MATCH (n:P) RETURN [b IN n.vals WHERE b > 1 | b]");
        std::map<std::string, std::string> var_map{{"b", "c"}};
        rewrite_expr_vars(q.returns[0].expr, var_map);
        std::set<std::string> names;
        const auto* lc = static_cast<const ListComprehensionExpr*>(q.returns[0].expr.get());
        collect_names(lc->filter.get(), names);
        REQUIRE(names.count("b") == 1);
        REQUIRE(names.count("c") == 0);
    }
}

TEST_CASE("output_row_cap bounds a page without proving the scan non-reducing", "[gql_optimizer]") {
    auto cap = [](const std::string& q) {
        return output_row_cap(GqlParser::parse(q));
    };
    SECTION("a plain LIMIT caps at the limit") {
        REQUIRE(cap("MATCH (p:Person) RETURN p.name LIMIT 10") == std::optional<uint64_t>(10));
    }
    SECTION("an OFFSET caps at the whole page window, not the bare limit") {
        REQUIRE(cap("MATCH (p:Person) RETURN p.name OFFSET 25 LIMIT 10") == std::optional<uint64_t>(35));
    }
    SECTION("a residual WHERE still caps -- filtered rows are never produced") {
        // The same query disqualifies the SCAN bound, which is exactly the case this cap unlocks.
        const std::string q = "MATCH (p:Person) WHERE p.age > 30 RETURN p.name LIMIT 10";
        REQUIRE(has_post_scan_residual_predicate(GqlParser::parse(q)));
        REQUIRE(cap(q) == std::optional<uint64_t>(10));
    }
    SECTION("a reducing join still caps -- no mandatory-relation proof needed") {
        // limit pushdown leaves this query's scan unbounded because it cannot prove the second match
        // mandatory; counting produced rows needs no such proof.
        REQUIRE(cap("MATCH (p:Person) MATCH (p)-[:SHIPPED_FROM]->(l:Location) RETURN p.name LIMIT 10")
                == std::optional<uint64_t>(10));
    }
    SECTION("ORDER BY is not capped: the last input row can sort first") {
        REQUIRE_FALSE(cap("MATCH (p:Person) RETURN p.name ORDER BY p.name LIMIT 10").has_value());
    }
    SECTION("DISTINCT is not capped: dedup collapses rows after projection") {
        REQUIRE_FALSE(cap("MATCH (p:Person) RETURN DISTINCT p.name LIMIT 10").has_value());
    }
    SECTION("an aggregate is not capped: many rows fold into few") {
        REQUIRE_FALSE(cap("MATCH (p:Person) RETURN count(p) AS c LIMIT 10").has_value());
    }
    SECTION("no LIMIT means no cap") {
        REQUIRE_FALSE(cap("MATCH (p:Person) RETURN p.name").has_value());
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
