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

#include "GqlExecutor.h"
#include "GqlValue.h"
#include "GqlWriter.h"
#include "GqlCatalog.h"
#include "GqlVirtualCatalog.h"
#include "GqlTypechecker.h"
#include "Join.h"
#include "GqlParser.h"
#include "GqlOptimizer.h"
#include "GqlQueryCache.h"
#include "optimizer/OptimizerUtils.h"
#include "executor/FactorNode.h"
#include "executor/JoinHelpers.h"
#include "executor/ExpressionEvaluator.h"
#include "executor/PathTraverser.h"
#include "../graph/cache/WccCache.h"
#include "../graph/cache/TransitiveReachabilityCache.h"
#include "executor/ProjectionPruner.h"
#include "executor/StarJoinRewriter.h"
#include "executor/PlanBuilder.h"
#include "executor/HoneycombExecutor.h"
#include "executor/LftjExecutor.h"

#include <sstream>
#include <cmath>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <map>
#include <algorithm>
#include <limits>
#include <cstdlib>
#include <chrono>
#include <seastar/core/when_all.hh>
#include <seastar/core/loop.hh>

namespace ragedb::gql {

/**
 * @brief Represents a grouped set of rows.
 */
struct GqlGroup {
    GqlRow representative;
    std::vector<GqlRow> rows;
};

/**
 * @brief Custom vector comparator for GqlValues to group mapping.
 */
struct GqlValueVectorLess {
    bool operator()(const std::vector<GqlValue>& a, const std::vector<GqlValue>& b) const {
        if (a.size() != b.size()) return a.size() < b.size();
        for (size_t i = 0; i < a.size(); ++i) {
            int cmp = compare_gql_values(a[i], b[i]);
            if (cmp != 0) return cmp < 0;
        }
        return false;
    }
};

/**
 * @brief Helper sorting structure representing a row mapped to its sort keys.
 */
struct RowSortKey {
    std::vector<GqlValue> keys;
    GqlRow row;
};

/**
 * @brief Main execution entry point for the GQL engine.
 */
struct QueryResult {
    std::vector<std::string> column_names;
    std::vector<std::vector<GqlValue>> rows;
    bool is_sorted = false;
    /// OFFSET/LIMIT already applied by the path that produced these rows, so the terminal must not page
    /// again -- a second LIMIT would be harmless, but a second OFFSET would skip a further offset rows.
    /// Set operations leave this false: they combine un-paged branch results and page the combination.
    bool is_paged = false;
};

/**
 * @brief Rows that an INTERMEDIATE truncation has to retain for a later OFFSET/LIMIT page to still have
 *        its window: the page keeps [offset, offset + limit), so cutting down to limit alone would throw
 *        away exactly the rows the offset skips past. Returns nullopt when nothing bounds the row count.
 */
static std::optional<uint64_t> page_window(const GqlQuery& q) {
    if (!q.limit) return std::nullopt;
    const uint64_t skip = q.offset.value_or(0);
    if (skip > std::numeric_limits<uint64_t>::max() - *q.limit) return std::nullopt;
    return skip + *q.limit;
}

/**
 * @brief Applies the OFFSET/LIMIT page to a FINAL row set: drops the first offset rows, then caps the rest
 *        at limit. Ordering and DISTINCT must already have been applied, since the page is defined over the
 *        ordered, deduplicated result.
 */
template <typename Row>
static void apply_page(std::vector<Row>& rows, const GqlQuery& q) {
    const size_t skip = static_cast<size_t>(q.offset.value_or(0));
    if (skip > 0) {
        if (skip >= rows.size()) {
            rows.clear();
        } else {
            rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(skip));
        }
    }
    if (q.limit && rows.size() > static_cast<size_t>(*q.limit)) {
        rows.resize(static_cast<size_t>(*q.limit));
    }
}

/**
 * @brief Computes STDDEV_POP or STDDEV_SAMP over the aggregated rows: the standard deviation of the
 *        expression's numeric values. Population divides the summed squared deviations by n, sample by
 *        n-1. NULL for no numeric input, and additionally for a single value in the sample form (n-1 = 0).
 */
static void collect_numeric(const GqlValue& v, std::vector<double>& out) {
    if (v.type != GqlValue::PROPERTY) return;
    if (std::holds_alternative<int64_t>(v.property)) out.push_back(static_cast<double>(std::get<int64_t>(v.property)));
    else if (std::holds_alternative<double>(v.property)) out.push_back(std::get<double>(v.property));
}

static GqlValue stddev_of(const std::vector<double>& values, bool sample) {
    const size_t n = values.size();
    if (n == 0 || (sample && n < 2)) return GqlValue();
    double mean = 0.0;
    for (double x : values) mean += x;
    mean /= static_cast<double>(n);
    double sq = 0.0;
    for (double x : values) { const double d = x - mean; sq += d * d; }
    const double denom = sample ? static_cast<double>(n - 1) : static_cast<double>(n);
    return GqlValue(std::sqrt(sq / denom));
}

static GqlValue compute_stddev(const std::vector<GqlRow>& rows, const Expression* expr, bool sample) {
    std::vector<double> values;
    for (const auto& r : rows) collect_numeric(evaluate_expression(r, expr), values);
    return stddev_of(values, sample);
}

/**
 * @brief Sorts the combined query results according to GQL ORDER BY specification.
 * @param res The QueryResult containing column names and rows.
 * @param order_by The sorting specifications.
 * @param limit Optional limit constraint to restrict result size. Pass the page window (offset + limit),
 *        not the bare limit, when an OFFSET follows -- the sort prunes, and the skipped rows are needed.
 */
static void sort_combined_result(QueryResult& res, const std::vector<SortSpec>& order_by, std::optional<size_t> limit = std::nullopt) {
    if (order_by.empty()) return;

    auto comp = [&res, &order_by](const std::vector<GqlValue>& a, const std::vector<GqlValue>& b) {
        for (const auto& spec : order_by) {
            size_t col_idx = res.column_names.size();
            if (spec.expr->kind == ExpressionKind::VARIABLE) {
                auto* ve = static_cast<const VariableExpr*>(spec.expr.get());
                for (size_t i = 0; i < res.column_names.size(); ++i) {
                    if (res.column_names[i] == ve->name) {
                        col_idx = i;
                        break;
                    }
                }
            } else if (spec.expr->kind == ExpressionKind::PROPERTY_LOOKUP) {
                auto* pl = static_cast<const PropertyLookupExpr*>(spec.expr.get());
                std::string col_name = pl->variable + "." + pl->property;
                for (size_t i = 0; i < res.column_names.size(); ++i) {
                    if (res.column_names[i] == col_name) {
                        col_idx = i;
                        break;
                    }
                }
            }
            
            if (col_idx < res.column_names.size()) {
                int cmp = compare_gql_values(a[col_idx], b[col_idx]);
                if (cmp != 0) {
                    return spec.ascending ? (cmp < 0) : (cmp > 0);
                }
            }
        }
        return false;
    };

    if (limit && *limit < res.rows.size()) {
        std::partial_sort(res.rows.begin(), res.rows.begin() + *limit, res.rows.end(), comp);
        res.rows.resize(*limit);
    } else {
        std::stable_sort(res.rows.begin(), res.rows.end(), comp);
    }
}

struct GqlRowOuterVarsLess {
    std::set<std::string> outer_vars;
    bool operator()(const GqlRow& lhs, const GqlRow& rhs) const {
        for (const auto& var : outer_vars) {
            auto it_l = lhs.bindings.find(var);
            auto it_r = rhs.bindings.find(var);
            GqlValue val_l = (it_l != lhs.bindings.end()) ? it_l->second : GqlValue();
            GqlValue val_r = (it_r != rhs.bindings.end()) ? it_r->second : GqlValue();
            int cmp = compare_gql_values(val_l, val_r);
            if (cmp < 0) return true;
            if (cmp > 0) return false;
        }
        return false;
    }
};
/**
 * @brief Internally executes a parsed GQL query against the database graph,
 * managing joins, projections, filtering, and aggregation.
 * @param graph The database graph instance.
 * @param query_ptr Shared pointer to the GQL query.
 * @return A future resolving to a QueryResult containing columns and values.
 */
/**
 * @brief Derive the output column name for a RETURN item. The single source of truth for column
 *        naming: query_result_to_rows turns column names into next-segment bindings in WITH
 *        pipelines, so every path that names columns must agree.
 */
static std::string return_item_column_name(const ReturnItem& item, size_t index) {
    if (item.alias) return *item.alias;
    if (item.expr->kind == ExpressionKind::PROPERTY_LOOKUP) {
        auto* pl = static_cast<const PropertyLookupExpr*>(item.expr.get());
        return pl->variable + "." + pl->property;
    }
    if (item.expr->kind == ExpressionKind::VARIABLE) {
        return static_cast<const VariableExpr*>(item.expr.get())->name;
    }
    if (item.expr->kind == ExpressionKind::AGGREGATION) {
        auto* ae = static_cast<const AggregateExpr*>(item.expr.get());
        std::string fn = ae->fn_kind == AggregateKind::COUNT ? "count"
            : ae->fn_kind == AggregateKind::SUM ? "sum"
            : ae->fn_kind == AggregateKind::AVG ? "avg"
            : ae->fn_kind == AggregateKind::MIN ? "min"
            : ae->fn_kind == AggregateKind::COLLECT ? "collect_list"
            : ae->fn_kind == AggregateKind::STDDEV_POP ? "stddev_pop"
            : ae->fn_kind == AggregateKind::STDDEV_SAMP ? "stddev_samp" : "max";
        if (!ae->expr) return fn + "(*)";
        if (ae->expr->kind == ExpressionKind::PROPERTY_LOOKUP) {
            auto* pl = static_cast<const PropertyLookupExpr*>(ae->expr.get());
            return fn + "(" + pl->variable + "." + pl->property + ")";
        }
        if (ae->expr->kind == ExpressionKind::VARIABLE) {
            return fn + "(" + static_cast<const VariableExpr*>(ae->expr.get())->name + ")";
        }
        return fn + "(expr)";
    }
    return "column_" + std::to_string(index);
}

/**
 * @brief A plan for answering a pure COUNT over a single node pattern from the shard count
 *        indexes, without materializing any Node objects.
 *
 * This is the fast path for queries shaped like `MATCH (n:Label) RETURN count(n)` (optionally with
 * a single property filter). Counting by materializing every node OOMs on large labels, so when a
 * query matches this exact shape we answer it directly with AllNodesCountPeered / FindNodeCountPeered.
 * Anything more complex falls back to the normal executor.
 */
struct SimpleNodeCountPlan {
    bool has_label = false;
    std::string label;
    bool has_filter = false;
    std::string filter_property;
    Operation filter_op = Operation::EQ;
    property_type_t filter_value;
    std::string column_name;
    uint64_t multiplier = 1;
};

/**
 * @brief Detect whether a query is a pure COUNT over a single node pattern and, if so, populate a
 *        SimpleNodeCountPlan. Returns false (fall back to the normal executor) for any shape that
 *        cannot be answered by a single count-index lookup.
 */
static bool try_plan_simple_node_count(const GqlQuery& q, SimpleNodeCountPlan& out) {
    if (q.kind != QueryKind::SINGLE) return false;
    if (q.explain || q.profile) return false;      // keep plan/profile output on the normal path
    if (!q.writes.empty()) return false;
    if (q.where_expr) return false;
    if (q.has_unnested_subquery) return false;
    if (q.distinct) return false;
    if (q.limit.has_value()) return false;
    if (q.offset.has_value()) return false;   // this plan emits the count row directly; nothing pages it
    if (!q.order_by.empty()) return false;
    if (q.matches.size() != 1) return false;
    if (q.returns.size() != 1) return false;

    const auto& item = q.returns[0];
    if (!item.expr || item.expr->kind != ExpressionKind::AGGREGATION) return false;
    const auto* ae = static_cast<const AggregateExpr*>(item.expr.get());
    if (ae->fn_kind != AggregateKind::COUNT) return false;

    const auto& m = q.matches[0];
    if (m.is_optional) return false;
    if (m.shortest_path_kind != ShortestPathKind::NONE) return false;
    if (m.is_khop) return false;
    if (!m.path_variable.empty()) return false;
    if (m.limit.has_value()) return false;
    if (!m.pattern.edges.empty()) return false;
    if (m.pattern.nodes.size() != 1) return false;

    const auto& node = m.pattern.nodes[0];
    if (node.where_expr) return false;
    // A property map value that is an expression is only known once it is resolved against the incoming
    // row, which this plan bypasses; counting through it would silently ignore the constraint.
    if (!node.property_exprs.empty()) return false;

    // The count target must be count(*) or count(<the pattern node's variable>). For a single
    // non-optional node every matched row binds a distinct node, so count(n) == row count.
    if (ae->expr) {
        if (ae->expr->kind != ExpressionKind::VARIABLE) return false;
        const auto* ve = static_cast<const VariableExpr*>(ae->expr.get());
        if (node.variable.empty() || ve->name != node.variable) return false;
    }

    // Only a single literal label (or no label) can be answered by the count endpoints;
    // AND/OR/NOT/WILDCARD label expressions fall back.
    if (node.label_expr) {
        if (node.label_expr->kind != LabelExprKind::LITERAL) return false;
        out.has_label = true;
        out.label = node.label_expr->name;
    }

    // Zero filters -> count all (of the label). Exactly one filter (with a label) -> FindNodeCount.
    // More than one filter has no single count endpoint, so fall back.
    size_t filter_count = node.properties.size() + node.property_filters.size();
    if (filter_count > 1) return false;
    if (filter_count == 1) {
        if (!out.has_label) return false;  // FindNodeCountPeered requires a node type
        out.has_filter = true;
        if (!node.properties.empty()) {
            const auto& kv = *node.properties.begin();
            out.filter_property = kv.first;
            out.filter_op = Operation::EQ;
            out.filter_value = kv.second;
        } else {
            const auto& f = node.property_filters[0];
            out.filter_property = f.property;
            out.filter_op = f.op;
            out.filter_value = f.value;
        }
    }

    out.column_name = return_item_column_name(item, 0);

    out.multiplier = q.count_multiplication_factor;
    return true;
}

/**
 * @brief Plan for a group-anchored streaming aggregate over a single-edge expansion.
 *
 * Answers queries shaped like `MATCH (a:AL)-[:R]->(b:BL) RETURN b.key, count(a) ...` by iterating
 * the group-side (b) nodes and folding each one's R-neighbours (a) into per-group accumulators, so
 * peak memory is O(#groups + one node's degree) instead of O(#expansion rows). This avoids the OOM
 * from materialising the whole (a,b) expansion just to aggregate it. Any shape not cleanly covered
 * returns false and falls back to the normal (materialising) executor.
 */
struct EdgeAggPlan {
    const PatternNode* group_node = nullptr;
    const PatternNode* agg_node = nullptr;
    std::string group_var;
    std::string agg_var;
    std::string agg_label;            // literal label of the aggregated node ("" = any)
    std::string rel_type;
    Direction dir = Direction::BOTH;  // from group node toward aggregated node
    std::vector<const Expression*> grouping_keys;
    std::vector<const AggregateExpr*> aggs;
    uint64_t multiplier = 1;
};

static bool node_label_literal(const PatternNode& n, std::string& out_label) {
    if (!n.label_expr) { out_label = ""; return true; }
    if (n.label_expr->kind == LabelExprKind::LITERAL) { out_label = n.label_expr->name; return true; }
    return false;
}

static bool plan_streaming_edge_aggregate(const GqlQuery& q, EdgeAggPlan& out) {
    if (q.kind != QueryKind::SINGLE) return false;
    if (q.explain || q.profile) return false;
    if (!q.writes.empty()) return false;
    if (q.where_expr) return false;
    if (q.has_unnested_subquery) return false;
    if (q.distinct) return false;
    if (q.matches.size() != 1) return false;

    const auto& m = q.matches[0];
    if (m.is_optional) return false;
    if (m.shortest_path_kind != ShortestPathKind::NONE) return false;
    if (m.is_khop) return false;
    if (!m.path_variable.empty()) return false;
    if (m.limit.has_value()) return false;
    if (m.pattern.nodes.size() != 2 || m.pattern.edges.size() != 1) return false;

    const auto& edge = m.pattern.edges[0];
    if (edge.is_variable_length) return false;
    if (edge.where_expr) return false;
    if (!edge.properties.empty() || !edge.property_filters.empty()) return false;
    if (!edge.label_expr || edge.label_expr->kind != LabelExprKind::LITERAL) return false;
    out.rel_type = edge.label_expr->name;

    // Property map values that are expressions are only known once resolved against the incoming row,
    // which this plan bypasses; planning through them would silently drop the constraint.
    for (const auto& n : m.pattern.nodes) {
        if (!n.property_exprs.empty()) return false;
    }
    if (!edge.property_exprs.empty()) return false;

    for (const auto& item : q.returns) find_aggregates(item.expr.get(), out.aggs);
    for (const auto& spec : q.order_by) find_aggregates(spec.expr.get(), out.aggs);
    if (out.aggs.empty()) return false;

    // DISTINCT aggregates (e.g. count(DISTINCT x)) need per-group value dedup, which the streaming
    // accumulator does not perform; collect_list gathers a list the accumulator does not build. Both
    // fall back to the normal grouped aggregation path.
    for (const auto* agg : out.aggs) {
        if (agg->distinct) return false;
        // COLLECT and STDDEV need the whole value set (a list, or sum-of-squares), which the streaming
        // accumulator does not carry, so they take the materialising path.
        if (agg->fn_kind == AggregateKind::COLLECT ||
            agg->fn_kind == AggregateKind::STDDEV_POP || agg->fn_kind == AggregateKind::STDDEV_SAMP) {
            return false;
        }
    }

    // Aggregates must be count(*) or over a single variable / one of its properties.
    std::string agg_var;
    for (const auto* agg : out.aggs) {
        if (!agg->expr) continue;  // count(*)
        std::string v;
        if (agg->expr->kind == ExpressionKind::VARIABLE) v = static_cast<const VariableExpr*>(agg->expr.get())->name;
        else if (agg->expr->kind == ExpressionKind::PROPERTY_LOOKUP) v = static_cast<const PropertyLookupExpr*>(agg->expr.get())->variable;
        else return false;
        if (v.empty()) return false;
        if (agg_var.empty()) agg_var = v;
        else if (agg_var != v) return false;  // aggregates span two variables
    }

    const auto& n0 = m.pattern.nodes[0];
    const auto& n1 = m.pattern.nodes[1];
    if (n0.variable.empty() || n1.variable.empty()) return false;
    // A self-loop pattern (both endpoints share one variable) needs the source==target join the
    // streaming runner never applies; it would count every edge instead of self-loops only.
    if (n0.variable == n1.variable) return false;

    const PatternNode* group_node = nullptr;
    const PatternNode* agg_node = nullptr;
    if (!agg_var.empty()) {
        if (agg_var == n0.variable) { agg_node = &n0; group_node = &n1; }
        else if (agg_var == n1.variable) { agg_node = &n1; group_node = &n0; }
        else return false;
    }

    // Grouping keys: the non-aggregate RETURN items; must reference a single variable.
    std::set<std::string> nonagg_vars;
    for (const auto& item : q.returns) {
        if (has_aggregates(item.expr.get())) continue;
        std::string v;
        if (item.expr->kind == ExpressionKind::VARIABLE) v = static_cast<const VariableExpr*>(item.expr.get())->name;
        else if (item.expr->kind == ExpressionKind::PROPERTY_LOOKUP) v = static_cast<const PropertyLookupExpr*>(item.expr.get())->variable;
        else return false;
        if (v.empty()) return false;
        nonagg_vars.insert(v);
        out.grouping_keys.push_back(item.expr.get());
    }
    if (nonagg_vars.size() > 1) return false;

    if (!group_node) {
        // pure count(*): choose the group node from the grouping keys, else default to n1.
        if (!nonagg_vars.empty()) {
            const std::string& gv = *nonagg_vars.begin();
            if (gv == n0.variable) { group_node = &n0; agg_node = &n1; }
            else if (gv == n1.variable) { group_node = &n1; agg_node = &n0; }
            else return false;
        } else {
            group_node = &n1; agg_node = &n0;
        }
    }
    if (!nonagg_vars.empty() && *nonagg_vars.begin() != group_node->variable) return false;

    std::string group_label, agg_label;
    if (!node_label_literal(*group_node, group_label)) return false;
    if (!node_label_literal(*agg_node, agg_label)) return false;
    if (!group_node->properties.empty() || !group_node->property_filters.empty() || group_node->where_expr) return false;
    if (!agg_node->properties.empty() || !agg_node->property_filters.empty() || agg_node->where_expr) return false;

    // Direction from group node toward aggregated node. RIGHT means nodes[0]->nodes[1].
    bool group_is_n0 = (group_node == &n0);
    if (edge.direction == EdgeDirection::ANY) out.dir = Direction::BOTH;
    else if (edge.direction == EdgeDirection::RIGHT) out.dir = group_is_n0 ? Direction::OUT : Direction::IN;
    else out.dir = group_is_n0 ? Direction::IN : Direction::OUT;

    // Non-aggregate ORDER BY expressions are evaluated against the group representative, which
    // binds only the group variable; a sort key on any other variable would evaluate to NIL and
    // order the groups arbitrarily (dropping the wrong ones under LIMIT).
    for (const auto& spec : q.order_by) {
        if (has_aggregates(spec.expr.get())) continue;
        std::string v;
        if (spec.expr->kind == ExpressionKind::VARIABLE) v = static_cast<const VariableExpr*>(spec.expr.get())->name;
        else if (spec.expr->kind == ExpressionKind::PROPERTY_LOOKUP) v = static_cast<const PropertyLookupExpr*>(spec.expr.get())->variable;
        else return false;
        if (v != group_node->variable) return false;
    }

    out.group_node = group_node;
    out.agg_node = agg_node;
    out.group_var = group_node->variable;
    out.agg_var = agg_node->variable;
    out.agg_label = agg_label;
    out.multiplier = q.count_multiplication_factor;
    return true;
}

/**
 * @brief Incremental accumulator for one aggregate over one group--mirrors the batch fold in the
 *        normal aggregate path so results are byte-for-byte identical.
 */
struct EdgeAggAccumulator {
    AggregateKind kind;
    const Expression* expr;  // nullptr for count(*)
    bool distinct = false;        // DISTINCT aggregate: fold over the distinct-value set instead
    bool count_to_sum = false;    // COUNT rewritten to a degree SUM: empty input yields 0, not null
    int64_t count = 0;
    int64_t sum_int = 0;
    double sum_double = 0.0;
    bool has_double = false;
    int64_t val_count = 0;
    GqlValue extreme;
    bool extreme_set = false;
    std::set<GqlValue, GqlValueLess> distinct_vals;
    std::unordered_set<uint64_t> distinct_node_ids;
    std::unordered_set<uint64_t> distinct_rel_ids;

    static EdgeAggAccumulator make(const AggregateExpr* agg) {
        EdgeAggAccumulator acc{agg->fn_kind, agg->expr.get()};
        acc.distinct = agg->distinct && agg->expr != nullptr;
        acc.count_to_sum = agg->count_to_sum;
        return acc;
    }

    void add(const GqlRow& row) {
        if (distinct) {
            GqlValue v = evaluate_expression(row, expr);
            if (v.type == GqlValue::NIL) return;
            // count(DISTINCT node/rel) only needs cardinality and entities dedup by id, so store
            // raw ids: a GqlValue set costs ~10x more per entry, which is the difference between
            // fitting and bad_alloc for multi-million-entry distinct sets at SF1.
            if (kind == AggregateKind::COUNT && v.type == GqlValue::NODE) {
                distinct_node_ids.insert(v.node->getId());
                return;
            }
            if (kind == AggregateKind::COUNT && v.type == GqlValue::RELATIONSHIP) {
                distinct_rel_ids.insert(v.relationship->getId());
                return;
            }
            distinct_vals.insert(std::move(v));
            return;
        }
        if (kind == AggregateKind::COUNT) {
            if (!expr) { count++; return; }
            if (evaluate_expression(row, expr).type != GqlValue::NIL) count++;
            return;
        }
        GqlValue v = evaluate_expression(row, expr);
        if (kind == AggregateKind::SUM || kind == AggregateKind::AVG) {
            if (v.type == GqlValue::PROPERTY) {
                if (std::holds_alternative<int64_t>(v.property)) {
                    if (has_double) sum_double += static_cast<double>(std::get<int64_t>(v.property));
                    else sum_int += std::get<int64_t>(v.property);
                    val_count++;
                } else if (std::holds_alternative<double>(v.property)) {
                    if (!has_double) { sum_double = static_cast<double>(sum_int); has_double = true; }
                    sum_double += std::get<double>(v.property);
                    val_count++;
                }
            }
        } else if (kind == AggregateKind::MIN) {
            if (v.type != GqlValue::NIL && (!extreme_set || compare_gql_values(v, extreme) < 0)) { extreme = v; extreme_set = true; }
        } else if (kind == AggregateKind::MAX) {
            if (v.type != GqlValue::NIL && (!extreme_set || compare_gql_values(v, extreme) > 0)) { extreme = v; extreme_set = true; }
        }
    }

    GqlValue finalize(uint64_t multiplier) const {
        if (distinct) return finalize_distinct(multiplier);
        if (kind == AggregateKind::COUNT) {
            int64_t c = count;
            if (multiplier > 1) c *= static_cast<int64_t>(multiplier);
            return GqlValue(c);
        }
        if (kind == AggregateKind::SUM) {
            if (val_count == 0) return count_to_sum ? GqlValue(static_cast<int64_t>(0)) : GqlValue();
            return has_double ? GqlValue(sum_double) : GqlValue(sum_int);
        }
        if (kind == AggregateKind::AVG) {
            if (val_count == 0) return GqlValue();
            return has_double ? GqlValue(sum_double / static_cast<double>(val_count))
                              : GqlValue(static_cast<double>(sum_int) / static_cast<double>(val_count));
        }
        return extreme_set ? extreme : GqlValue();
    }

    GqlValue finalize_distinct(uint64_t multiplier) const {
        if (kind == AggregateKind::COUNT) {
            int64_t c = static_cast<int64_t>(distinct_vals.size() + distinct_node_ids.size() + distinct_rel_ids.size());
            if (multiplier > 1) c *= static_cast<int64_t>(multiplier);
            return GqlValue(c);
        }
        if (kind == AggregateKind::MIN) return distinct_vals.empty() ? GqlValue() : *distinct_vals.begin();
        if (kind == AggregateKind::MAX) return distinct_vals.empty() ? GqlValue() : *distinct_vals.rbegin();
        int64_t s_int = 0;
        double s_double = 0.0;
        bool dbl = false;
        int64_t n = 0;
        for (const auto& v : distinct_vals) {
            if (v.type != GqlValue::PROPERTY) continue;
            if (std::holds_alternative<int64_t>(v.property)) {
                if (dbl) s_double += static_cast<double>(std::get<int64_t>(v.property));
                else s_int += std::get<int64_t>(v.property);
                n++;
            } else if (std::holds_alternative<double>(v.property)) {
                if (!dbl) { s_double = static_cast<double>(s_int); dbl = true; }
                s_double += std::get<double>(v.property);
                n++;
            }
        }
        if (n == 0) return GqlValue();
        if (kind == AggregateKind::SUM) return dbl ? GqlValue(s_double) : GqlValue(s_int);
        double total = dbl ? s_double : static_cast<double>(s_int);
        return GqlValue(total / static_cast<double>(n));
    }
};

struct EdgeAggGroupState {
    bool initialized = false;
    GqlRow rep;
    std::vector<EdgeAggAccumulator> accs;
};

struct EdgeAggRunState {
    std::map<std::vector<GqlValue>, EdgeAggGroupState, GqlValueVectorLess> groups;
};

/**
 * @brief Execute a group-anchored streaming aggregate (see EdgeAggPlan). Iterates the group-side
 *        nodes, folds each one's aggregated-side neighbours into per-group accumulators, then
 *        projects/sorts/limits the (small) group set exactly as the normal aggregate path does.
 */
static seastar::future<QueryResult> run_streaming_edge_aggregate(
        ragedb::Graph& graph, std::shared_ptr<GqlQuery> query_ptr, EdgeAggPlan plan) {
    auto state = std::make_shared<EdgeAggRunState>();
    auto plan_ptr = std::make_shared<EdgeAggPlan>(std::move(plan));

    // Keep the group node's properties intact (grouping keys read them); the default pruner would
    // strip every property.
    ProjectionPruner group_pruner;
    group_pruner.whole_objects.insert(plan_ptr->group_var);

    // Whether any accumulator evaluates an expression against the row: pure count(*) folds never
    // look at the neighbour binding, so the per-neighbour row write is skipped entirely for them.
    const bool needs_neighbor_binding = std::any_of(plan_ptr->aggs.begin(), plan_ptr->aggs.end(),
        [](const AggregateExpr* a) { return a->expr != nullptr; });

    return get_start_nodes(graph, *plan_ptr->group_node, 0, group_pruner)
    .then([&graph, state, plan_ptr, needs_neighbor_binding](std::vector<Node> group_nodes) {
        auto nodes = std::make_shared<std::vector<Node>>(std::move(group_nodes));
        // Bounded fan-out instead of one-at-a-time: each group's neighbour fetch is an RPC, and
        // awaiting them serially leaves the reactor idle for num_groups x latency.
        return seastar::max_concurrent_for_each(nodes->begin(), nodes->end(), 32,
            [&graph, state, plan_ptr, needs_neighbor_binding](Node& g) {
                uint64_t gid = g.getId();
                return graph.shard.local().NodeGetNeighborsPeered(gid, plan_ptr->dir, plan_ptr->rel_type)
                .then([state, plan_ptr, needs_neighbor_binding, g = std::move(g)](std::vector<Node> neighbors) {
                    const auto& plan = *plan_ptr;
                    GqlRow rep;
                    rep.bindings[plan.group_var] = GqlValue(g);

                    bool any = false;
                    EdgeAggGroupState* gstate = nullptr;
                    GqlRow row;  // reused across neighbours: only the aggregated binding changes
                    for (auto& n : neighbors) {
                        if (!plan.agg_label.empty() && n.getType() != plan.agg_label) continue;
                        if (!any) {
                            std::vector<GqlValue> key;
                            for (const auto* gk : plan.grouping_keys) key.push_back(evaluate_expression(rep, gk));
                            gstate = &state->groups[key];
                            if (!gstate->initialized) {
                                gstate->rep = rep;
                                for (const auto* agg : plan.aggs) gstate->accs.push_back(EdgeAggAccumulator::make(agg));
                                gstate->initialized = true;
                            }
                            if (needs_neighbor_binding) row = rep;
                            any = true;
                        }
                        if (needs_neighbor_binding) {
                            row.bindings[plan.agg_var] = GqlValue(std::move(n));
                        }
                        for (auto& acc : gstate->accs) acc.add(row);
                    }
                });
            }
        ).then([state, plan_ptr, nodes] { (void)nodes; });
    }).then([query_ptr, state, plan_ptr] {
        const auto& query = *query_ptr;
        const auto& plan = *plan_ptr;

        QueryResult result;
        for (size_t i = 0; i < query.returns.size(); ++i) {
            result.column_names.push_back(return_item_column_name(query.returns[i], i));
        }

        // Ungrouped aggregate over an empty expansion still yields one row (e.g. count -> 0).
        if (state->groups.empty() && plan.grouping_keys.empty()) {
            EdgeAggGroupState empty_group;
            empty_group.rep = GqlRow{};
            for (const auto* agg : plan.aggs) empty_group.accs.push_back(EdgeAggAccumulator::make(agg));
            empty_group.initialized = true;
            state->groups[{}] = std::move(empty_group);
        }

        struct GroupRow { std::vector<GqlValue> projected; std::vector<GqlValue> sort_keys; };
        std::vector<GroupRow> rows;
        for (auto& [key, gstate] : state->groups) {
            (void)key;
            std::map<const AggregateExpr*, GqlValue> agg_results;
            for (size_t i = 0; i < plan.aggs.size(); ++i) {
                agg_results[plan.aggs[i]] = gstate.accs[i].finalize(plan.multiplier);
            }
            GroupRow gr;
            for (const auto& item : query.returns) {
                gr.projected.push_back(evaluate_group_expression(gstate.rep, agg_results, item.expr.get()));
            }
            for (const auto& spec : query.order_by) {
                gr.sort_keys.push_back(evaluate_group_expression(gstate.rep, agg_results, spec.expr.get()));
            }
            rows.push_back(std::move(gr));
        }

        const auto window = page_window(query);
        if (!query.order_by.empty()) {
            auto comp = [&query](const GroupRow& a, const GroupRow& b) {
                for (size_t i = 0; i < query.order_by.size(); ++i) {
                    int cmp = compare_gql_values(a.sort_keys[i], b.sort_keys[i]);
                    if (cmp != 0) return query.order_by[i].ascending ? (cmp < 0) : (cmp > 0);
                }
                return false;
            };
            if (window && *window < rows.size()) {
                std::partial_sort(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(*window), rows.end(), comp);
                rows.resize(static_cast<size_t>(*window));
            } else {
                std::stable_sort(rows.begin(), rows.end(), comp);
            }
        } else if (window && *window < rows.size()) {
            rows.resize(static_cast<size_t>(*window));
        }
        apply_page(rows, query);
        result.is_paged = true;

        for (auto& gr : rows) result.rows.push_back(std::move(gr.projected));
        return result;
    });
}

static seastar::future<QueryResult> execute_query_internal(ragedb::Graph& graph, std::shared_ptr<GqlQuery> query_ptr, std::optional<std::vector<GqlRow>> incoming = std::nullopt);

/**
 * @brief Convert a completed segment's QueryResult into input rows for the next WITH-pipeline
 *        segment: each projected output column becomes a binding under its column name. Takes the
 *        result by value and moves every cell, so the pipe holds one copy of the data, not two.
 */
static std::vector<GqlRow> query_result_to_rows(QueryResult res) {
    std::vector<GqlRow> rows;
    rows.reserve(res.rows.size());
    for (auto& r : res.rows) {
        GqlRow gr;
        for (size_t i = 0; i < res.column_names.size() && i < r.size(); ++i) {
            gr.bindings[res.column_names[i]] = std::move(r[i]);
        }
        rows.push_back(std::move(gr));
    }
    return rows;
}

/**
 * @brief Execute WITH-pipeline segments [idx..], piping each one's projected rows into the next, and
 *        return the rows to feed into the final (RETURN) segment. An empty segment still pipes its
 *        (empty) rows onward: a later ungrouped aggregate over zero rows must yield one row (count 0).
 */
static seastar::future<std::vector<GqlRow>> execute_with_segments(
        ragedb::Graph& graph, std::shared_ptr<GqlQuery> query_ptr, size_t idx,
        std::optional<std::vector<GqlRow>> incoming) {
    if (idx >= query_ptr->with_segments.size()) {
        return seastar::make_ready_future<std::vector<GqlRow>>(
            incoming.has_value() ? std::move(*incoming) : std::vector<GqlRow>{});
    }
    auto seg = query_ptr->with_segments[idx];
    return execute_query_internal(graph, seg, std::move(incoming)).then(
        [&graph, query_ptr, idx](QueryResult res) {
            std::vector<GqlRow> rows = query_result_to_rows(std::move(res));
            return execute_with_segments(graph, query_ptr, idx + 1,
                std::optional<std::vector<GqlRow>>(std::move(rows)));
        });
}

/**
 * @brief Whether a query's single match statement can be driven through the streaming traversal
 *        sink: a plain (non-optional, non-search, non-fast-path) pattern with at least one edge
 *        and nothing that needs the materialised row set as a whole. The streamed shapes are the
 *        expansion-heavy ones that OOM when collected (tasks 018/020).
 */
static bool match_streamable(const MatchStatement& m) {
    if (m.is_optional || m.is_search || m.is_khop) return false;
    if (m.shortest_path_kind != ShortestPathKind::NONE) return false;
    if (m.algebraic_path_count || m.khop_count_only) return false;
    if (m.equivalence_partition_lookup || m.transitive_reachability_lookup) return false;
    if (!m.path_variable.empty()) return false;
    if (m.limit.has_value()) return false;
    // A plain node scan with no edges streams too: its rows are the scanned nodes themselves, which the
    // traversal pages and hands to the sink. Excluding it sent every aggregate or top-K over a bare label
    // (a date-filtered count of Posts, say) down the materialising path, which holds the whole label --
    // every node WITH its properties -- in one vector before the fold ever runs.
    return true;
}

static bool stream_eligible(const GqlQuery& q) {
    if (q.kind != QueryKind::SINGLE) return false;
    if (q.explain || q.profile) return false;
    if (!q.writes.empty()) return false;
    if (q.has_unnested_subquery) return false;
    if (q.matches.size() != 1) return false;
    if (!q.let_bindings.empty()) return false; // LET adds computed columns; use the materialising path
    if (!q.for_bindings.empty()) return false; // FOR multiplies rows; the streamed heap counts matches
    return match_streamable(q.matches[0]);
}

/**
 * @brief Whether a grouped/ungrouped aggregate can be streamed through a MULTI-match chain: every
 *        match is individually streamable and the segment carries no shape that needs the whole
 *        materialised row set. This bounds the FoF + expansion + count(DISTINCT) class (tasks 018/029)
 *        that OOM-crashes when the full (frontier x expansion) join is collected before aggregating.
 *        Cross-match DIFFERENT-EDGES uniqueness is enforced on the completed row in the fold sink.
 */
static bool stream_group_eligible(const GqlQuery& q) {
    if (q.kind != QueryKind::SINGLE) return false;
    if (q.explain || q.profile) return false;
    if (!q.writes.empty()) return false;
    if (q.has_unnested_subquery) return false;
    if (!q.let_bindings.empty()) return false;
    if (!q.for_bindings.empty()) return false; // FOR multiplies rows before the aggregate folds them
    if (q.matches.empty()) return false;
    for (const auto& m : q.matches) {
        if (!match_streamable(m)) return false;
    }
    return true;
}

/**
 * @brief Streamed ORDER BY + LIMIT: fold every matched row into a bounded top-K heap as the
 *        traversal produces it, so peak memory is K rows plus one traversal chunk instead of the
 *        whole expansion (the IC2/IC9 OOM class). Row-level filters (path modes, residual WHERE)
 *        are applied before the heap; the projection runs over the K winners at the end.
 */
static seastar::future<QueryResult> run_streaming_topk(
        ragedb::Graph& graph, std::shared_ptr<GqlQuery> query_ptr,
        std::optional<std::vector<GqlRow>> incoming, const ProjectionPruner& pruner) {
    struct TopKEntry {
        std::vector<GqlValue> keys;
        GqlRow row;
    };
    struct TopKState {
        std::vector<TopKEntry> heap;
        size_t k = 0;
    };
    auto st = std::make_shared<TopKState>();
    // The heap must retain the whole page window (offset + limit): an OFFSET skips past the first rows of
    // the ordered result, so keeping only `limit` of them would discard exactly what the page returns.
    st->k = static_cast<size_t>(page_window(*query_ptr).value_or(*query_ptr->limit));

    // Orders entries by the ORDER BY specs; the heap is a max-heap under this comparator, so its
    // front is the current worst kept entry.
    auto entry_less = [query_ptr](const TopKEntry& a, const TopKEntry& b) {
        for (size_t i = 0; i < query_ptr->order_by.size(); ++i) {
            int cmp = compare_gql_values(a.keys[i], b.keys[i]);
            if (cmp != 0) return query_ptr->order_by[i].ascending ? (cmp < 0) : (cmp > 0);
        }
        return false;
    };

    auto sink = std::make_shared<GqlRowSink>();
    sink->consume = [st, query_ptr, entry_less](std::vector<GqlRow> rows) {
        const auto& stmt = query_ptr->matches[0];
        for (auto& r : rows) {
            if (!satisfies_match_path_modes(r, stmt.match_mode, stmt.path_mode, stmt.pattern)) continue;
            if (query_ptr->where_expr && !evaluate_expression(r, query_ptr->where_expr.get()).is_truthy()) continue;
            TopKEntry e;
            e.keys.reserve(query_ptr->order_by.size());
            for (const auto& spec : query_ptr->order_by) {
                e.keys.push_back(evaluate_expression(r, spec.expr.get()));
            }
            if (st->heap.size() < st->k) {
                e.row = std::move(r);
                st->heap.push_back(std::move(e));
                std::push_heap(st->heap.begin(), st->heap.end(), entry_less);
            } else if (st->k > 0 && entry_less(e, st->heap.front())) {
                e.row = std::move(r);
                std::pop_heap(st->heap.begin(), st->heap.end(), entry_less);
                st->heap.back() = std::move(e);
                std::push_heap(st->heap.begin(), st->heap.end(), entry_less);
            }
        }
        return seastar::make_ready_future<>();
    };

    auto rows_in = std::make_shared<std::vector<GqlRow>>(
        incoming.has_value() ? std::move(*incoming) : std::vector<GqlRow>{ GqlRow{} });
    return seastar::max_concurrent_for_each(rows_in->begin(), rows_in->end(), gql_stream_concurrency,
        [&graph, query_ptr, pruner, sink](GqlRow& in) {
            const auto& stmt = query_ptr->matches[0];
            return traverse_path_pattern(graph, stmt.pattern, in, 0, pruner, "", true, false,
                                         stmt.path_mode, sink.get()).discard_result();
        })
    .then([st, query_ptr, entry_less, rows_in, sink] {
        std::sort(st->heap.begin(), st->heap.end(), entry_less);
        apply_page(st->heap, *query_ptr);
        QueryResult res;
        res.is_sorted = true;
        res.is_paged = true;
        for (size_t i = 0; i < query_ptr->returns.size(); ++i) {
            res.column_names.push_back(return_item_column_name(query_ptr->returns[i], i));
        }
        for (auto& e : st->heap) {
            std::vector<GqlValue> projected;
            projected.reserve(query_ptr->returns.size());
            for (const auto& item : query_ptr->returns) {
                projected.push_back(evaluate_expression(e.row, item.expr.get()));
            }
            res.rows.push_back(std::move(projected));
        }
        return res;
    });
}

/**
 * @brief Collect the user-named edge variables that participate in a DIFFERENT EDGES match (GQL's
 *        default). Auto-generated (`_`-prefixed) edge variables are excluded, matching the batch path.
 */
static std::set<std::string> collect_different_edge_vars(const GqlQuery& q) {
    std::set<std::string> vars;
    for (const auto& stmt : q.matches) {
        if (stmt.match_mode == MatchMode::DIFFERENT_EDGES) {
            for (const auto& edge : stmt.pattern.edges) {
                if (!edge.variable.empty() && edge.variable[0] != '_') vars.insert(edge.variable);
            }
        }
    }
    return vars;
}

/** @brief Whether the relationships bound to the DIFFERENT-EDGES variables in a completed row are all
 *         distinct (the cross-match GQL uniqueness the batch path enforces at PathTraverser.cpp). */
static bool row_edges_all_distinct(const GqlRow& row, const std::set<std::string>& diff_edge_vars) {
    if (diff_edge_vars.empty()) return true;
    std::vector<uint64_t> rel_ids;
    for (const auto& var : diff_edge_vars) {
        auto it = row.bindings.find(var);
        if (it == row.bindings.end()) continue;
        const auto& val = it->second;
        if (val.type == GqlValue::RELATIONSHIP) {
            rel_ids.push_back(val.relationship->getId());
        } else if (val.type == GqlValue::RELATIONSHIP_LIST) {
            for (const auto& r : *val.relationship_list) rel_ids.push_back(r.getId());
        } else if (val.type == GqlValue::PATH) {
            for (const auto& r : val.path->GetRelationships()) rel_ids.push_back(r.getId());
        }
    }
    std::set<uint64_t> uniq(rel_ids.begin(), rel_ids.end());
    return uniq.size() == rel_ids.size();
}

/**
 * @brief Stream one input row through matches[idx..], emitting each fully-joined row to final_sink.
 *        Cross-match binding works because traverse_path_pattern uses already-bound variables in the
 *        row as pattern anchors; each match's path/mode constraint is checked on its own output before
 *        driving the next match. This keeps a multi-match expansion (FoF -> forum -> post) bounded to
 *        one traversal chunk instead of the whole join.
 */
static seastar::future<> stream_match_chain(
        ragedb::Graph& graph, std::shared_ptr<GqlQuery> query_ptr,
        std::shared_ptr<const ProjectionPruner> pruner, size_t idx, GqlRow row,
        std::shared_ptr<GqlRowSink> final_sink) {
    if (idx >= query_ptr->matches.size()) {
        return final_sink->consume(std::vector<GqlRow>{ std::move(row) });
    }
    const auto& stmt = query_ptr->matches[idx];
    auto next_sink = std::make_shared<GqlRowSink>();
    next_sink->consume = [&graph, query_ptr, pruner, idx, final_sink,
                          mode = stmt.match_mode, pmode = stmt.path_mode,
                          pattern = &stmt.pattern](std::vector<GqlRow> rows) {
        auto rows_ptr = std::make_shared<std::vector<GqlRow>>(std::move(rows));
        return seastar::max_concurrent_for_each(*rows_ptr, gql_stream_inner_concurrency,
            [&graph, query_ptr, pruner, idx, final_sink, mode, pmode, pattern](GqlRow& r) {
                if (!satisfies_match_path_modes(r, mode, pmode, *pattern)) {
                    return seastar::make_ready_future<>();
                }
                return stream_match_chain(graph, query_ptr, pruner, idx + 1, std::move(r), final_sink);
            }).finally([rows_ptr] {});
    };
    return traverse_path_pattern(graph, stmt.pattern, row, 0, *pruner, "", true, false,
                                 stmt.path_mode, next_sink.get())
        .discard_result().finally([next_sink] {});
}

/**
 * @brief Streamed grouped aggregation over one or more match statements (with or without piped input
 *        rows): every fully-joined row folds into per-group incremental accumulators as it is
 *        produced, so peak memory is O(groups) instead of the whole expansion (the FoF DISTINCT ->
 *        expand -> aggregate crash class, tasks 018/029). Grouping keys are the non-aggregate RETURN
 *        items, the group representative is its first row, and the final project/sort/limit runs over
 *        the group set.
 */
static seastar::future<QueryResult> run_streaming_group_fold(
        ragedb::Graph& graph, std::shared_ptr<GqlQuery> query_ptr,
        std::optional<std::vector<GqlRow>> incoming, const ProjectionPruner& pruner,
        std::vector<const Expression*> grouping_keys, std::vector<const AggregateExpr*> aggs) {
    struct GroupState {
        GqlRow rep;
        std::vector<EdgeAggAccumulator> accs;
    };
    struct FoldState {
        std::map<std::vector<GqlValue>, GroupState, GqlValueVectorLess> groups;
    };
    auto st = std::make_shared<FoldState>();
    auto keys_ptr = std::make_shared<std::vector<const Expression*>>(std::move(grouping_keys));
    auto aggs_ptr = std::make_shared<std::vector<const AggregateExpr*>>(std::move(aggs));

    auto diff_edge_vars = std::make_shared<std::set<std::string>>(collect_different_edge_vars(*query_ptr));
    auto sink = std::make_shared<GqlRowSink>();
    sink->consume = [st, query_ptr, keys_ptr, aggs_ptr, diff_edge_vars](std::vector<GqlRow> rows) {
        for (auto& r : rows) {
            // Per-match path/mode constraints were checked in the chain; here the row is fully joined,
            // so apply the cross-match DIFFERENT-EDGES uniqueness and the segment WHERE, then fold.
            if (!row_edges_all_distinct(r, *diff_edge_vars)) continue;
            if (query_ptr->where_expr && !evaluate_expression(r, query_ptr->where_expr.get()).is_truthy()) continue;
            std::vector<GqlValue> key;
            key.reserve(keys_ptr->size());
            for (const auto* gk : *keys_ptr) key.push_back(evaluate_expression(r, gk));
            auto [it, inserted] = st->groups.try_emplace(std::move(key));
            if (inserted) {
                it->second.rep = r;
                it->second.accs.reserve(aggs_ptr->size());
                for (const auto* agg : *aggs_ptr) it->second.accs.push_back(EdgeAggAccumulator::make(agg));
            }
            for (auto& acc : it->second.accs) acc.add(r);
        }
        return seastar::make_ready_future<>();
    };

    auto pruner_ptr = std::make_shared<const ProjectionPruner>(pruner);
    auto rows_in = std::make_shared<std::vector<GqlRow>>(
        incoming.has_value() ? std::move(*incoming) : std::vector<GqlRow>{ GqlRow{} });
    return seastar::max_concurrent_for_each(rows_in->begin(), rows_in->end(), gql_stream_concurrency,
        [&graph, query_ptr, pruner_ptr, sink](GqlRow& in) {
            return stream_match_chain(graph, query_ptr, pruner_ptr, 0, in, sink);
        })
    .then([st, query_ptr, keys_ptr, aggs_ptr, rows_in, sink] {
        const auto& query = *query_ptr;

        QueryResult result;
        for (size_t i = 0; i < query.returns.size(); ++i) {
            result.column_names.push_back(return_item_column_name(query.returns[i], i));
        }

        // Ungrouped aggregate over an empty expansion still yields one row (e.g. count -> 0).
        if (st->groups.empty() && keys_ptr->empty()) {
            GroupState empty_group;
            empty_group.rep = GqlRow{};
            for (const auto* agg : *aggs_ptr) empty_group.accs.push_back(EdgeAggAccumulator::make(agg));
            st->groups[{}] = std::move(empty_group);
        }

        struct GroupRow {
            std::vector<GqlValue> projected;
            std::vector<GqlValue> sort_keys;
        };
        std::vector<GroupRow> rows;
        rows.reserve(st->groups.size());
        for (auto& [key, gstate] : st->groups) {
            (void)key;
            std::map<const AggregateExpr*, GqlValue> agg_results;
            for (size_t i = 0; i < aggs_ptr->size(); ++i) {
                agg_results[(*aggs_ptr)[i]] = gstate.accs[i].finalize(query.count_multiplication_factor);
            }
            GroupRow gr;
            for (const auto& item : query.returns) {
                gr.projected.push_back(evaluate_group_expression(gstate.rep, agg_results, item.expr.get()));
            }
            for (const auto& spec : query.order_by) {
                gr.sort_keys.push_back(evaluate_group_expression(gstate.rep, agg_results, spec.expr.get()));
            }
            rows.push_back(std::move(gr));
        }

        const auto window = page_window(query);
        if (!query.order_by.empty()) {
            auto comp = [&query](const GroupRow& a, const GroupRow& b) {
                for (size_t i = 0; i < query.order_by.size(); ++i) {
                    int cmp = compare_gql_values(a.sort_keys[i], b.sort_keys[i]);
                    if (cmp != 0) return query.order_by[i].ascending ? (cmp < 0) : (cmp > 0);
                }
                return false;
            };
            if (window && *window < rows.size()) {
                std::partial_sort(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(*window), rows.end(), comp);
                rows.resize(static_cast<size_t>(*window));
            } else {
                std::stable_sort(rows.begin(), rows.end(), comp);
            }
        } else if (window && *window < rows.size()) {
            rows.resize(static_cast<size_t>(*window));
        }
        apply_page(rows, query);
        result.is_paged = true;

        for (auto& gr : rows) result.rows.push_back(std::move(gr.projected));
        return result;
    });
}

static seastar::future<QueryResult> execute_query_internal(ragedb::Graph& graph, std::shared_ptr<GqlQuery> query_ptr, std::optional<std::vector<GqlRow>> incoming) {
    // WITH continuation segments carry piped rows; planners that answer from indexes or run the
    // pattern from scratch cannot consume them and must be skipped (they would ignore the pipe).
    const bool has_incoming = incoming.has_value();
    if (query_ptr->no_op) {
        QueryResult query_res;
        for (size_t i = 0; i < query_ptr->returns.size(); ++i) {
            query_res.column_names.push_back(return_item_column_name(query_ptr->returns[i], i));
        }
        return seastar::make_ready_future<QueryResult>(std::move(query_res));
    }

    // WITH pipeline: execute the prefix segments (each projecting rows forward), then run the final
    // RETURN segment on the piped rows. The final segment is this query with its with_segments removed.
    if (!query_ptr->with_segments.empty()) {
        return execute_with_segments(graph, query_ptr, 0, std::move(incoming))
        .then([&graph, query_ptr](std::vector<GqlRow> piped) {
            auto final_seg = std::make_shared<GqlQuery>(query_ptr->clone());
            final_seg->with_segments.clear();
            return execute_query_internal(graph, final_seg, std::optional<std::vector<GqlRow>>(std::move(piped)));
        });
    }

    // 1. Handle Set Operations (UNION, INTERSECT, EXCEPT, each with an optional ALL)
    if (query_ptr->kind != QueryKind::SINGLE) {
        // Release ownership of subqueries to execute them separately
        auto left_ptr = std::shared_ptr<GqlQuery>(query_ptr->left.release());
        auto right_ptr = std::shared_ptr<GqlQuery>(query_ptr->right.release());

        // Extract left and right plan nodes if profiling is enabled
        std::shared_ptr<PlanNode> left_node = nullptr;
        std::shared_ptr<PlanNode> right_node = nullptr;
        if (query_ptr->profile && query_ptr->plan_root) {
            if (query_ptr->plan_root->children.size() > 0) {
                left_node = query_ptr->plan_root->children[0];
            }
            if (query_ptr->plan_root->children.size() > 1) {
                right_node = query_ptr->plan_root->children[1];
            }
        }

        // Set up profiling contexts for subqueries
        if (query_ptr->profile) {
            left_ptr->profile = true;
            left_ptr->plan_root = left_node;
            index_plan_nodes(left_node, left_ptr->plan_nodes);

            right_ptr->profile = true;
            right_ptr->plan_root = right_node;
            index_plan_nodes(right_node, right_ptr->plan_nodes);
        }

        // Start timing the set operation
        auto start = std::chrono::steady_clock::now();
        
        // Execute the left subquery first
        return execute_query_internal(graph, left_ptr)
        .then([&graph, right_ptr, query_ptr, start](QueryResult left_res) {
            // Then execute the right subquery
            return execute_query_internal(graph, right_ptr)
            .then([left_res = std::move(left_res), query_ptr, start](QueryResult right_res) {
                // Ensure column structures match
                if (left_res.column_names.size() != right_res.column_names.size()) {
                    throw std::runtime_error("All subqueries in a GQL Set operation must return the same number of columns");
                }

                QueryResult combined_res;
                combined_res.column_names = left_res.column_names;

                // Merge results based on the set operation type
                if (query_ptr->kind == QueryKind::UNION_ALL) {
                    // UNION ALL: Concatenate all rows
                    combined_res.rows = std::move(left_res.rows);
                    for (auto& r : right_res.rows) {
                        combined_res.rows.push_back(std::move(r));
                    }
                } else if (query_ptr->kind == QueryKind::UNION) {
                    // UNION: Concatenate and dedup rows using a set
                    std::set<std::vector<GqlValue>, GqlValueVectorLess> seen;
                    for (auto& r : left_res.rows) {
                        if (seen.insert(r).second) {
                            combined_res.rows.push_back(std::move(r));
                        }
                    }
                    for (auto& r : right_res.rows) {
                        if (seen.insert(r).second) {
                            combined_res.rows.push_back(std::move(r));
                        }
                    }
                } else if (query_ptr->kind == QueryKind::INTERSECT) {
                    // INTERSECT: Retain unique rows present in both left and right results
                    std::set<std::vector<GqlValue>, GqlValueVectorLess> left_set;
                    for (const auto& r : left_res.rows) {
                        left_set.insert(r);
                    }
                    std::set<std::vector<GqlValue>, GqlValueVectorLess> seen_intersection;
                    for (auto& r : right_res.rows) {
                        if (left_set.count(r)) {
                            if (seen_intersection.insert(r).second) {
                                combined_res.rows.push_back(std::move(r));
                            }
                        }
                    }
                } else if (query_ptr->kind == QueryKind::INTERSECT_ALL) {
                    // INTERSECT ALL: Retain rows present in both, respecting multi-set count limits
                    std::map<std::vector<GqlValue>, int64_t, GqlValueVectorLess> left_counts;
                    for (const auto& r : left_res.rows) {
                        left_counts[r]++;
                    }
                    for (auto& r : right_res.rows) {
                        auto it = left_counts.find(r);
                        if (it != left_counts.end() && it->second > 0) {
                            combined_res.rows.push_back(std::move(r));
                            it->second--;
                        }
                    }
                } else if (query_ptr->kind == QueryKind::EXCEPT) {
                    // EXCEPT: distinct rows of the left that do not appear in the right.
                    std::set<std::vector<GqlValue>, GqlValueVectorLess> right_set;
                    for (const auto& r : right_res.rows) {
                        right_set.insert(r);
                    }
                    std::set<std::vector<GqlValue>, GqlValueVectorLess> emitted;
                    for (auto& r : left_res.rows) {
                        if (right_set.count(r)) continue;
                        if (emitted.insert(r).second) {
                            combined_res.rows.push_back(std::move(r));
                        }
                    }
                } else if (query_ptr->kind == QueryKind::EXCEPT_ALL) {
                    // EXCEPT ALL: multiset difference -- each right occurrence cancels one left occurrence.
                    std::map<std::vector<GqlValue>, int64_t, GqlValueVectorLess> right_counts;
                    for (const auto& r : right_res.rows) {
                        right_counts[r]++;
                    }
                    for (auto& r : left_res.rows) {
                        auto it = right_counts.find(r);
                        if (it != right_counts.end() && it->second > 0) {
                            it->second--;   // this left row is cancelled by a right occurrence
                            continue;
                        }
                        combined_res.rows.push_back(std::move(r));
                    }
                }

                // Record actual row count and duration for set operation plan node
                if (query_ptr->profile && query_ptr->plan_root) {
                    auto end = std::chrono::steady_clock::now();
                    query_ptr->plan_root->actual_rows = combined_res.rows.size();
                    query_ptr->plan_root->time_ms = std::chrono::duration<double, std::milli>(end - start).count();
                }

                return combined_res;
            });
        });
    }

    // Fast path: a pure COUNT over a single node pattern is answered directly from the shard count
    // indexes, without materializing Node objects--otherwise counting a large label (e.g.
    // count(Comment) over millions of rows) exhausts memory.
    {
        SimpleNodeCountPlan cplan;
        if (!has_incoming && query_ptr->let_bindings.empty() && query_ptr->for_bindings.empty() &&
            try_plan_simple_node_count(*query_ptr, cplan)) {
            seastar::future<uint64_t> count_fut = cplan.has_filter
                ? graph.shard.local().FindNodeCountPeered(cplan.label, cplan.filter_property, cplan.filter_op, cplan.filter_value)
                : (cplan.has_label ? graph.shard.local().AllNodesCountPeered(cplan.label)
                                   : graph.shard.local().AllNodesCountPeered());
            return count_fut.then([cplan](uint64_t c) {
                int64_t value = static_cast<int64_t>(c);
                if (cplan.multiplier > 1) {
                    value *= static_cast<int64_t>(cplan.multiplier);
                }
                QueryResult result;
                result.column_names.push_back(cplan.column_name);
                std::vector<GqlValue> row;
                row.push_back(GqlValue(value));
                result.rows.push_back(std::move(row));
                return result;
            });
        }
    }

    // Fast path: a grouped/ungrouped aggregate over a single-edge expansion is streamed group by
    // group (see EdgeAggPlan) instead of materialising the whole (a,b) expansion--otherwise a
    // large expansion (e.g. count(comment) per person over millions of comments) exhausts memory.
    {
        EdgeAggPlan eplan;
        if (!has_incoming && query_ptr->let_bindings.empty() && query_ptr->for_bindings.empty() &&
            plan_streaming_edge_aggregate(*query_ptr, eplan)) {
            return run_streaming_edge_aggregate(graph, query_ptr, std::move(eplan));
        }
    }

    // 2. Detect if the query contains aggregate functions (COUNT, SUM, AVG, MIN, MAX)
    bool query_contains_aggregates = false;
    for (const auto& item : query_ptr->returns) {
        if (has_aggregates(item.expr.get())) {
            query_contains_aggregates = true;
            break;
        }
    }
    if (!query_contains_aggregates) {
        for (const auto& spec : query_ptr->order_by) {
            if (has_aggregates(spec.expr.get())) {
                query_contains_aggregates = true;
                break;
            }
        }
    }

    // Determine if we can push limit evaluation directly into the traversal. Pushing the LIMIT as
    // the physical scan bound is only sound when every remaining predicate is applied INSIDE the
    // scan; if any predicate is evaluated (or rows are collapsed) after the scan, scanning exactly
    // `limit` rows under-returns. In those cases scan with the default bound and truncate. The
    // residual conditions live in has_post_scan_residual_predicate, shared with the
    // LimitPushdownOptimizer so the two gates cannot drift.
    size_t limit_val = 0;
    if (query_ptr->limit.has_value() && query_ptr->order_by.empty() &&
        !query_contains_aggregates && !has_post_scan_residual_predicate(*query_ptr)) {
        // Scan the whole page window: with an OFFSET, scanning only `limit` rows would stop short of the
        // rows the page actually returns.
        const auto window = page_window(*query_ptr);
        if (window) {
            limit_val = static_cast<size_t>(*window);
        }
    }

    // 3. Build ProjectionPruner to avoid reading properties that are never accessed
    ProjectionPruner pruner;

    // Collect properties accessed in RETURN expressions
    for (const auto& item : query_ptr->returns) {
        collect_accessed_properties(item.expr.get(), pruner.accessed_props, pruner.whole_objects);
    }

    // Collect properties accessed in ORDER BY specs
    for (const auto& spec : query_ptr->order_by) {
        collect_accessed_properties(spec.expr.get(), pruner.accessed_props, pruner.whole_objects);
    }

    // Collect properties accessed in ISO GQL LET binding expressions
    for (const auto& let : query_ptr->let_bindings) {
        collect_accessed_properties(let.expr.get(), pruner.accessed_props, pruner.whole_objects);
    }

    // Collect properties accessed in ISO GQL FOR list expressions
    for (const auto& binding : query_ptr->for_bindings) {
        collect_accessed_properties(binding.list_expr.get(), pruner.accessed_props, pruner.whole_objects);
    }

    // Collect properties accessed in WHERE filter
    if (query_ptr->where_expr) {
        collect_accessed_properties(query_ptr->where_expr.get(), pruner.accessed_props, pruner.whole_objects);
    }

    // Collect properties accessed/modified in WRITE operations
    for (const auto& w : query_ptr->writes) {
        if (w.type == WriteOp::Type::INSERT) {
            for (const auto& n : w.insert_pattern.nodes) {
                if (!n.variable.empty()) {
                    pruner.whole_objects.insert(n.variable);
                }
            }
            for (const auto& e : w.insert_pattern.edges) {
                if (!e.variable.empty()) {
                    pruner.whole_objects.insert(e.variable);
                }
            }
        } else if (w.type == WriteOp::Type::SET) {
            if (!w.set_var.empty()) {
                pruner.whole_objects.insert(w.set_var);
            }
            collect_accessed_properties(w.set_expr.get(), pruner.accessed_props, pruner.whole_objects);
        } else if (w.type == WriteOp::Type::REMOVE) {
            if (!w.remove_var.empty()) {
                pruner.whole_objects.insert(w.remove_var);
            }
        } else if (w.type == WriteOp::Type::DELETE_OP) {
            if (!w.delete_var.empty()) {
                pruner.whole_objects.insert(w.delete_var);
            }
        }
    }

    // Collect properties referenced in matching patterns (filters/indexed properties)
    for (const auto& match : query_ptr->matches) {
        for (const auto& n : match.pattern.nodes) {
            if (!n.variable.empty()) {
                for (const auto& [prop, val] : n.properties) {
                    pruner.accessed_props[n.variable].insert(prop);
                }
                for (const auto& [prop, expr] : n.property_exprs) {
                    pruner.accessed_props[n.variable].insert(prop);
                    collect_accessed_properties(expr.get(), pruner.accessed_props, pruner.whole_objects);
                }
                for (const auto& filter : n.property_filters) {
                    pruner.accessed_props[n.variable].insert(filter.property);
                }
            }
        }
        for (const auto& e : match.pattern.edges) {
            if (!e.variable.empty()) {
                for (const auto& [prop, val] : e.properties) {
                    pruner.accessed_props[e.variable].insert(prop);
                }
                for (const auto& [prop, expr] : e.property_exprs) {
                    pruner.accessed_props[e.variable].insert(prop);
                    collect_accessed_properties(expr.get(), pruner.accessed_props, pruner.whole_objects);
                }
                for (const auto& filter : e.property_filters) {
                    pruner.accessed_props[e.variable].insert(filter.property);
                }
            }
        }
    }

    // A variable used ONLY as a bare count(...) / count(DISTINCT ...) argument contributes just
    // its identity (node comparison and dedup are by id), so its properties can be pruned: a
    // count(DISTINCT post) over a large expansion otherwise carries every post's full property
    // map through traversal and into the distinct set. Skipped when a path variable is bound
    // (paths embed the nodes wholesale) or when writes are present.
    if (query_ptr->writes.empty() &&
        std::none_of(query_ptr->matches.begin(), query_ptr->matches.end(),
                     [](const MatchStatement& m) { return !m.path_variable.empty(); })) {
        std::vector<const AggregateExpr*> pruning_aggs;
        for (const auto& item : query_ptr->returns) find_aggregates(item.expr.get(), pruning_aggs);
        for (const auto& spec : query_ptr->order_by) find_aggregates(spec.expr.get(), pruning_aggs);
        for (const auto* agg : pruning_aggs) {
            if (agg->fn_kind != AggregateKind::COUNT || !agg->expr ||
                agg->expr->kind != ExpressionKind::VARIABLE) {
                continue;
            }
            const std::string& v = static_cast<const VariableExpr*>(agg->expr.get())->name;
            bool outside = query_ptr->where_expr &&
                is_variable_referenced_outside_count(query_ptr->where_expr.get(), v);
            for (const auto& item : query_ptr->returns) {
                if (outside) break;
                outside = is_variable_referenced_outside_count(item.expr.get(), v);
            }
            for (const auto& spec : query_ptr->order_by) {
                if (outside) break;
                outside = is_variable_referenced_outside_count(spec.expr.get(), v);
            }
            if (!outside) {
                pruner.whole_objects.erase(v);
                pruner.accessed_props.try_emplace(v);
            }
        }
    }

    // Streaming execution (tasks 018/020): expansion-heavy shapes fold matched rows as they are
    // produced instead of materialising the whole expansion, bounding peak memory.
    // Streaming top-K: a single-match ORDER BY + LIMIT without aggregates folds into a bounded heap.
    if (stream_eligible(*query_ptr) && !query_ptr->distinct &&
        !query_ptr->order_by.empty() && query_ptr->limit.has_value() && !query_contains_aggregates) {
        return run_streaming_topk(graph, query_ptr, std::move(incoming), pruner);
    }
    // Streaming grouped aggregate: one or more streamable matches, bounding the FoF -> expand ->
    // count(DISTINCT) crash class by folding each joined row into per-group accumulators.
    if (stream_group_eligible(*query_ptr) && !query_ptr->distinct && query_contains_aggregates) {
        // Mirrors the batch fold's key derivation: a bare returned variable groups by the whole
        // entity, so property lookups on it are not separate keys.
        std::set<std::string> group_variables;
        for (const auto& item : query_ptr->returns) {
            if (item.expr && !has_aggregates(item.expr.get()) && item.expr->kind == ExpressionKind::VARIABLE) {
                group_variables.insert(static_cast<const VariableExpr*>(item.expr.get())->name);
            }
        }
        std::vector<const Expression*> grouping_keys;
        for (const auto& item : query_ptr->returns) {
            if (has_aggregates(item.expr.get())) continue;
            if (item.expr->kind == ExpressionKind::PROPERTY_LOOKUP &&
                group_variables.count(static_cast<const PropertyLookupExpr*>(item.expr.get())->variable)) {
                continue;
            }
            grouping_keys.push_back(item.expr.get());
        }
        std::vector<const AggregateExpr*> aggs;
        for (const auto& item : query_ptr->returns) find_aggregates(item.expr.get(), aggs);
        for (const auto& spec : query_ptr->order_by) find_aggregates(spec.expr.get(), aggs);
        // collect_list gathers a list, and stddev needs a sum-of-squares, neither of which the streaming
        // accumulator builds; use the materialising group path for them.
        bool needs_materialising = false;
        for (const auto* a : aggs) {
            if (a->fn_kind == AggregateKind::COLLECT ||
                a->fn_kind == AggregateKind::STDDEV_POP || a->fn_kind == AggregateKind::STDDEV_SAMP) {
                needs_materialising = true;
                break;
            }
        }
        if (!aggs.empty() && !needs_materialising) {
            return run_streaming_group_fold(graph, query_ptr, std::move(incoming), pruner,
                                            std::move(grouping_keys), std::move(aggs));
        }
    }

    // 4. Optimize sorting if the order matches the start node traversal index order
    std::string sort_property = "";
    bool sort_ascending = true;
    bool sort_by_id = false;

    if (query_ptr->order_by.size() == 1 && !query_ptr->matches.empty()) {
        const auto& spec = query_ptr->order_by[0];
        std::string var_name = "";
        std::string prop_name = "";
        bool ok = false;
        
        if (spec.expr->kind == ExpressionKind::PROPERTY_LOOKUP) {
            auto* pl = static_cast<const PropertyLookupExpr*>(spec.expr.get());
            var_name = pl->variable;
            prop_name = pl->property;
            ok = true;
        } else if (spec.expr->kind == ExpressionKind::VARIABLE) {
            auto* ve = static_cast<const VariableExpr*>(spec.expr.get());
            var_name = ve->name;
            ok = true;
            sort_by_id = true;
        }
        
        if (ok) {
            const auto& first_match = query_ptr->matches[0];
            if (!first_match.pattern.nodes.empty()) {
                std::string start_var = first_match.pattern.nodes[0].variable;
                if (start_var == var_name || (start_var.empty() && var_name == "_n_0_user_empty")) {
                    sort_property = prop_name;
                    sort_ascending = spec.ascending;
                }
            }
        }
    }

    // 5. Execute matching patterns recursively using sharded traversal. WITH-pipeline continuation
    // segments start from the rows piped in from the previous segment rather than a single empty row.
    IntermediateResult initial_res = incoming.has_value()
        ? IntermediateResult(std::move(*incoming))
        : IntermediateResult(std::vector<GqlRow>{ GqlRow{} });
    auto is_sorted_shared = std::make_shared<bool>(false);

    auto is_query_cyclic = [](const std::vector<MatchStatement>& matches) -> bool {
        std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> adj_list;
        std::set<std::string> node_vars;
        
        for (size_t m_idx = 0; m_idx < matches.size(); ++m_idx) {
            const auto& match = matches[m_idx];
            if (match.is_search) continue;
            const auto& pattern = match.pattern;
            for (size_t i = 0; i < pattern.edges.size(); ++i) {
                std::string u = pattern.nodes[i].variable;
                std::string v = pattern.nodes[i+1].variable;
                std::string e = pattern.edges[i].variable;
                if (u.empty()) u = "_n_anon_" + std::to_string(m_idx) + "_" + std::to_string(i);
                if (v.empty()) v = "_n_anon_" + std::to_string(m_idx) + "_" + std::to_string(i+1);
                if (e.empty()) e = "_e_anon_" + std::to_string(m_idx) + "_" + std::to_string(i);
                
                node_vars.insert(u);
                node_vars.insert(v);
                adj_list[u].push_back({v, e});
                adj_list[v].push_back({u, e});
            }
        }
        
        std::unordered_set<std::string> visited;
        std::function<bool(const std::string&, const std::string&)> dfs = [&](const std::string& curr, const std::string& parent_edge) -> bool {
            visited.insert(curr);
            for (const auto& neighbor_info : adj_list[curr]) {
                std::string neighbor = neighbor_info.first;
                std::string edge_var = neighbor_info.second;
                if (edge_var == parent_edge) continue;
                if (visited.count(neighbor)) {
                    return true;
                }
                if (dfs(neighbor, edge_var)) {
                    return true;
                }
            }
            return false;
        };
        
        for (const auto& node : node_vars) {
            if (!visited.count(node)) {
                if (dfs(node, "")) return true;
            }
        }
        return false;
    };

    bool use_honeycomb = false;
    bool use_lftj = false;
    if (!has_incoming && is_query_cyclic(query_ptr->matches) && query_ptr->count_multiplication_factor <= 1) {
        uint64_t total_rels = 0;
        for (const auto& match : query_ptr->matches) {
            if (match.is_search) continue;
            for (const auto& edge : match.pattern.edges) {
                std::string rel_type = "";
                if (edge.label_expr && edge.label_expr->kind == LabelExprKind::LITERAL) {
                    rel_type = edge.label_expr->name;
                }
                if (!rel_type.empty()) {
                    total_rels += graph.shard.local().RelationshipCount(rel_type);
                } else {
                    total_rels += graph.shard.local().AllRelationshipsCount();
                }
            }
        }
        if (GqlExecutor::force_enable_honeycomb) {
            use_honeycomb = true;
        } else if (GqlExecutor::force_enable_lftj) {
            use_lftj = true;
        } else {
            if (total_rels > 10000) {
                if (!GqlExecutor::force_disable_honeycomb) {
                    use_honeycomb = true;
                } else if (!GqlExecutor::force_disable_lftj) {
                    use_lftj = true;
                }
            } else {
                if (!GqlExecutor::force_disable_lftj) {
                    use_lftj = true;
                } else if (!GqlExecutor::force_disable_honeycomb) {
                    use_honeycomb = true;
                }
            }
        }
    }

    auto hc_start = std::chrono::steady_clock::now();
    auto matched_fut = [&]() {
        if (use_honeycomb) {
            return HoneycombExecutor::execute(graph, query_ptr->matches, limit_val)
            .then([query_ptr, hc_start](IntermediateResult res) {
                if (query_ptr && query_ptr->profile) {
                    auto end = std::chrono::steady_clock::now();
                    auto node = query_ptr->plan_nodes["honeycomb_join"];
                    if (node) {
                        node->actual_rows = res.rows.size();
                        node->time_ms = std::chrono::duration<double, std::milli>(end - hc_start).count();
                    }
                }
                return res;
            });
        } else if (use_lftj) {
            return LftjExecutor::execute(graph, query_ptr->matches, limit_val)
            .then([query_ptr, hc_start](IntermediateResult res) {
                if (query_ptr && query_ptr->profile) {
                    auto end = std::chrono::steady_clock::now();
                    auto node = query_ptr->plan_nodes["lftj_join"];
                    if (node) {
                        node->actual_rows = res.rows.size();
                        node->time_ms = std::chrono::duration<double, std::milli>(end - hc_start).count();
                    }
                }
                return res;
            });
        } else {
            return execute_match_chain_factorized(graph, query_ptr->matches, 0, std::move(initial_res), limit_val, pruner, sort_property, sort_ascending, sort_by_id, query_ptr);
        }
    }();

    return matched_fut

    .then([&graph, query_ptr, is_sorted_shared](IntermediateResult matched_res) {
        *is_sorted_shared = matched_res.is_sorted;
        matched_res.ensure_flat();
        std::vector<GqlRow> matched_rows = std::move(matched_res.rows);
        const auto& query = *query_ptr;

        // 6. Enforce DIFFERENT EDGES match mode globally across joined rows (GQL semantics)
        std::set<std::string> diff_edge_vars;
        for (const auto& stmt : query.matches) {
            if (stmt.match_mode == MatchMode::DIFFERENT_EDGES) {
                for (const auto& edge : stmt.pattern.edges) {
                    if (!edge.variable.empty() && edge.variable[0] != '_') {
                        diff_edge_vars.insert(edge.variable);
                    }
                }
            }
        }

        if (!diff_edge_vars.empty()) {
            std::vector<GqlRow> edge_filtered_rows;
            for (auto& row : matched_rows) {
                std::vector<uint64_t> rel_ids;
                for (const auto& var : diff_edge_vars) {
                    auto it = row.bindings.find(var);
                    if (it != row.bindings.end()) {
                        const auto& val = it->second;
                        if (val.type == GqlValue::RELATIONSHIP) {
                            rel_ids.push_back(val.relationship->getId());
                        } else if (val.type == GqlValue::RELATIONSHIP_LIST) {
                            for (const auto& r : *val.relationship_list) {
                                rel_ids.push_back(r.getId());
                                }
                        } else if (val.type == GqlValue::PATH) {
                            for (const auto& r : val.path->GetRelationships()) {
                                rel_ids.push_back(r.getId());
                            }
                        }
                    }
                }
                std::set<uint64_t> unique_rels(rel_ids.begin(), rel_ids.end());
                if (unique_rels.size() == rel_ids.size()) {
                    edge_filtered_rows.push_back(std::move(row));
                }
            }
            matched_rows = std::move(edge_filtered_rows);
        }

        // 6a. Expand ISO GQL FOR (the standard's UNWIND) before anything else reads the working table:
        //     unlike LET, which adds a column to each row, FOR multiplies the rows -- one per element of
        //     the list. A pattern-less query (FOR x IN [1, 2, 3] RETURN x) still has a row to expand,
        //     because the match stage yields a single empty row when there is nothing to match.
        if (!query.for_bindings.empty()) {
            if (matched_rows.empty() && query.matches.empty()) {
                matched_rows.push_back(GqlRow{});
            }
            for (const auto& binding : query.for_bindings) {
                std::vector<GqlRow> expanded;
                for (const auto& row : matched_rows) {
                    const GqlValue list_val = evaluate_expression(row, binding.list_expr.get());
                    const auto elements = as_list_elements(list_val);
                    if (!elements) {
                        continue;   // a non-list (or null) expands to no rows, as an empty list does
                    }
                    for (const auto& element : *elements) {
                        GqlRow expanded_row = row;
                        expanded_row.bindings[binding.variable] = element;
                        expanded.push_back(std::move(expanded_row));
                    }
                }
                matched_rows = std::move(expanded);
            }
        }

        // 6b. Evaluate ISO GQL LET bindings against each matched row so the segment's WHERE/FILTER,
        //     ORDER BY, and RETURN can reference them (LET adds computed columns to the working table).
        if (!query.let_bindings.empty()) {
            for (auto& row : matched_rows) {
                for (const auto& let : query.let_bindings) {
                    if (let.alias) {
                        row.bindings[*let.alias] = evaluate_expression(row, let.expr.get());
                    }
                }
            }
        }

        // 7. Evaluate GQL WHERE clause filter expression on match results
        std::vector<GqlRow> filtered_rows;
        auto filter_start = std::chrono::steady_clock::now();
        for (auto& row : matched_rows) {
            if (!query.where_expr || evaluate_expression(row, query.where_expr.get()).is_truthy()) {
                filtered_rows.push_back(std::move(row));
            }
        }

        // Record profile timing for filter operator
        if (query_ptr->profile && query.where_expr) {
            auto filter_end = std::chrono::steady_clock::now();
            auto node = query_ptr->plan_nodes["filter"];
            if (node) {
                node->actual_rows = filtered_rows.size();
                node->time_ms = std::chrono::duration<double, std::milli>(filter_end - filter_start).count();
            }
        }

        // Deduplicate rows if querying with unnested subqueries
        if (query.has_unnested_subquery && !query.outer_vars.empty()) {
            std::vector<GqlRow> deduped_rows;
            std::set<GqlRow, GqlRowOuterVarsLess> seen((GqlRowOuterVarsLess{query.outer_vars}));
            for (auto& row : filtered_rows) {
                if (seen.insert(row).second) {
                    deduped_rows.push_back(std::move(row));
                }
            }
            filtered_rows = std::move(deduped_rows);
        }


        if (query.writes.empty()) {
            return seastar::make_ready_future<std::vector<GqlRow>>(std::move(filtered_rows));
        }

        auto write_start = std::chrono::steady_clock::now();
        std::vector<seastar::future<GqlRow>> futs;
        for (auto& row : filtered_rows) {
            futs.push_back(execute_writes_for_row(graph, query_ptr, 0, std::move(row)));
        }
        return seastar::when_all_succeed(futs.begin(), futs.end())
        .then([query_ptr, write_start](std::vector<GqlRow> written_rows) {
            if (query_ptr->profile) {
                auto write_end = std::chrono::steady_clock::now();
                auto node = query_ptr->plan_nodes["write"];
                if (node) {
                    node->actual_rows = written_rows.size();
                    node->time_ms = std::chrono::duration<double, std::milli>(write_end - write_start).count();
                }
            }
            return written_rows;
        });
    })
    .then([query_ptr, is_sorted_shared](std::vector<GqlRow> written_rows) {
        const auto& query = *query_ptr;
        std::vector<GqlRow> filtered_rows = std::move(written_rows);

        bool contains_aggregates = false;
        for (const auto& item : query.returns) {
            if (has_aggregates(item.expr.get())) {
                contains_aggregates = true;
                break;
            }
        }
        if (!contains_aggregates) {
            for (const auto& spec : query.order_by) {
                if (has_aggregates(spec.expr.get())) {
                    contains_aggregates = true;
                    break;
                }
            }
        }

        QueryResult query_res;
        query_res.is_sorted = *is_sorted_shared;

        for (size_t i = 0; i < query.returns.size(); ++i) {
            query_res.column_names.push_back(return_item_column_name(query.returns[i], i));
        }

        std::unordered_set<std::string> seen_distinct;

        if (contains_aggregates) {
            auto agg_start = std::chrono::steady_clock::now();
            std::vector<const AggregateExpr*> aggregate_exprs;
            for (const auto& item : query.returns) {
                find_aggregates(item.expr.get(), aggregate_exprs);
            }
            for (const auto& spec : query.order_by) {
                find_aggregates(spec.expr.get(), aggregate_exprs);
            }

            std::set<std::string> group_variables;
            for (const auto& item : query.returns) {
                if (item.expr && !has_aggregates(item.expr.get())) {
                    if (item.expr->kind == ExpressionKind::VARIABLE) {
                        group_variables.insert(static_cast<const VariableExpr*>(item.expr.get())->name);
                    }
                }
            }

            std::vector<const Expression*> grouping_keys;
            for (const auto& item : query.returns) {
                if (item.expr && !has_aggregates(item.expr.get())) {
                    if (item.expr->kind == ExpressionKind::PROPERTY_LOOKUP) {
                        auto* pl = static_cast<const PropertyLookupExpr*>(item.expr.get());
                        if (group_variables.count(pl->variable)) {
                            continue;
                        }
                    }
                    grouping_keys.push_back(item.expr.get());
                }
            }

            std::map<std::vector<GqlValue>, GqlGroup, GqlValueVectorLess> groups;
            for (auto& row : filtered_rows) {
                std::vector<GqlValue> key;
                for (const auto* gk : grouping_keys) {
                    key.push_back(evaluate_expression(row, gk));
                }
                auto& group = groups[key];
                if (group.rows.empty()) {
                    group.representative = row;
                }
                group.rows.push_back(std::move(row));
            }

            if (groups.empty() && grouping_keys.empty()) {
                GqlGroup default_group;
                default_group.representative = GqlRow{};
                groups[{}] = default_group;
            }

            struct GroupSortKey {
                std::vector<GqlValue> sort_keys;
                std::vector<GqlValue> projected_row;
            };
            std::vector<GroupSortKey> sorted_groups;

            for (const auto& [key, group] : groups) {
                std::map<const AggregateExpr*, GqlValue> aggregate_results;
                for (const auto* agg : aggregate_exprs) {
                    // A DISTINCT aggregate folds over the set of distinct non-null values directly:
                    // the values are evaluated exactly once and no rows are copied. The set is
                    // ordered by GqlValueLess (compare_gql_values), so MIN/MAX are its endpoints.
                    if (agg->distinct && agg->expr) {
                        std::set<GqlValue, GqlValueLess> distinct_vals;
                        for (const auto& r : group.rows) {
                            GqlValue v = evaluate_expression(r, agg->expr.get());
                            if (v.type != GqlValue::NIL) distinct_vals.insert(std::move(v));
                        }
                        if (agg->fn_kind == AggregateKind::COUNT) {
                            int64_t count = static_cast<int64_t>(distinct_vals.size());
                            if (query.count_multiplication_factor > 1) {
                                count *= query.count_multiplication_factor;
                            }
                            aggregate_results[agg] = GqlValue(count);
                        } else if (agg->fn_kind == AggregateKind::MIN) {
                            aggregate_results[agg] = distinct_vals.empty() ? GqlValue() : *distinct_vals.begin();
                        } else if (agg->fn_kind == AggregateKind::MAX) {
                            aggregate_results[agg] = distinct_vals.empty() ? GqlValue() : *distinct_vals.rbegin();
                        } else if (agg->fn_kind == AggregateKind::COLLECT) {
                            aggregate_results[agg] = GqlValue(std::vector<GqlValue>(distinct_vals.begin(), distinct_vals.end()));
                        } else if (agg->fn_kind == AggregateKind::STDDEV_POP || agg->fn_kind == AggregateKind::STDDEV_SAMP) {
                            std::vector<double> values;
                            for (const auto& v : distinct_vals) collect_numeric(v, values);
                            aggregate_results[agg] = stddev_of(values, agg->fn_kind == AggregateKind::STDDEV_SAMP);
                        } else {  // SUM / AVG with int->double promotion, mirroring the row fold below
                            int64_t sum_int = 0;
                            double sum_double = 0.0;
                            bool has_double = false;
                            int64_t count = 0;
                            for (const auto& v : distinct_vals) {
                                if (v.type != GqlValue::PROPERTY) continue;
                                if (std::holds_alternative<int64_t>(v.property)) {
                                    if (has_double) sum_double += static_cast<double>(std::get<int64_t>(v.property));
                                    else sum_int += std::get<int64_t>(v.property);
                                    count++;
                                } else if (std::holds_alternative<double>(v.property)) {
                                    if (!has_double) {
                                        sum_double = static_cast<double>(sum_int);
                                        has_double = true;
                                    }
                                    sum_double += std::get<double>(v.property);
                                    count++;
                                }
                            }
                            if (count == 0) {
                                aggregate_results[agg] = GqlValue();
                            } else if (agg->fn_kind == AggregateKind::SUM) {
                                aggregate_results[agg] = has_double ? GqlValue(sum_double) : GqlValue(sum_int);
                            } else {
                                double total = has_double ? sum_double : static_cast<double>(sum_int);
                                aggregate_results[agg] = GqlValue(total / static_cast<double>(count));
                            }
                        }
                        continue;
                    }
                    const std::vector<GqlRow>& agg_rows = group.rows;
                    if (agg->fn_kind == AggregateKind::COUNT) {
                        int64_t count = 0;
                        if (!agg->expr) {
                            count = static_cast<int64_t>(group.rows.size());
                        } else {
                            for (const auto& r : agg_rows) {
                                GqlValue v = evaluate_expression(r, agg->expr.get());
                                if (v.type != GqlValue::NIL) {
                                    count++;
                                }
                            }
                        }
                        if (query.count_multiplication_factor > 1) {
                            count *= query.count_multiplication_factor;
                        }
                        aggregate_results[agg] = GqlValue(count);
                    } else if (agg->fn_kind == AggregateKind::SUM || agg->fn_kind == AggregateKind::AVG) {
                        int64_t sum_int = 0;
                        double sum_double = 0.0;
                        bool has_double = false;
                        int64_t count = 0;

                        for (const auto& r : agg_rows) {
                            GqlValue v = evaluate_expression(r, agg->expr.get());
                            if (v.type == GqlValue::PROPERTY) {
                                if (std::holds_alternative<int64_t>(v.property)) {
                                    if (has_double) sum_double += static_cast<double>(std::get<int64_t>(v.property));
                                    else sum_int += std::get<int64_t>(v.property);
                                    count++;
                                } else if (std::holds_alternative<double>(v.property)) {
                                    if (!has_double) {
                                        sum_double = static_cast<double>(sum_int);
                                        has_double = true;
                                    }
                                    sum_double += std::get<double>(v.property);
                                    count++;
                                }
                            }
                        }

                        if (agg->fn_kind == AggregateKind::SUM) {
                            if (count == 0) {
                                // A COUNT rewritten into a degree SUM keeps count semantics over
                                // empty input: 0, not the null a genuine sum would produce.
                                aggregate_results[agg] = agg->count_to_sum ? GqlValue(static_cast<int64_t>(0)) : GqlValue();
                            } else if (has_double) {
                                aggregate_results[agg] = GqlValue(sum_double);
                            } else {
                                aggregate_results[agg] = GqlValue(sum_int);
                            }
                        } else {
                            if (count == 0) {
                                aggregate_results[agg] = GqlValue();
                            } else if (has_double) {
                                aggregate_results[agg] = GqlValue(sum_double / static_cast<double>(count));
                            } else {
                                aggregate_results[agg] = GqlValue(static_cast<double>(sum_int) / static_cast<double>(count));
                            }
                        }
                    } else if (agg->fn_kind == AggregateKind::MIN) {
                        GqlValue min_val;
                        for (const auto& r : agg_rows) {
                            GqlValue v = evaluate_expression(r, agg->expr.get());
                            if (v.type != GqlValue::NIL) {
                                if (min_val.type == GqlValue::NIL || compare_gql_values(v, min_val) < 0) {
                                    min_val = v;
                                }
                            }
                        }
                        aggregate_results[agg] = min_val;
                    } else if (agg->fn_kind == AggregateKind::MAX) {
                        GqlValue max_val;
                        for (const auto& r : agg_rows) {
                            GqlValue v = evaluate_expression(r, agg->expr.get());
                            if (v.type != GqlValue::NIL) {
                                if (max_val.type == GqlValue::NIL || compare_gql_values(v, max_val) > 0) {
                                    max_val = v;
                                }
                            }
                        }
                        aggregate_results[agg] = max_val;
                    } else if (agg->fn_kind == AggregateKind::COLLECT) {
                        std::vector<GqlValue> items;
                        for (const auto& r : agg_rows) {
                            GqlValue v = evaluate_expression(r, agg->expr.get());
                            if (v.type != GqlValue::NIL) items.push_back(std::move(v));
                        }
                        aggregate_results[agg] = GqlValue(std::move(items));
                    } else if (agg->fn_kind == AggregateKind::STDDEV_POP || agg->fn_kind == AggregateKind::STDDEV_SAMP) {
                        aggregate_results[agg] = compute_stddev(agg_rows, agg->expr.get(),
                                                                agg->fn_kind == AggregateKind::STDDEV_SAMP);
                    }
                }

                std::vector<GqlValue> projected;
                for (size_t i = 0; i < query.returns.size(); ++i) {
                    const auto& item = query.returns[i];
                    projected.push_back(evaluate_group_expression(group.representative, aggregate_results, item.expr.get()));
                }

                GroupSortKey gsk;
                gsk.projected_row = std::move(projected);
                for (const auto& spec : query.order_by) {
                    gsk.sort_keys.push_back(evaluate_group_expression(group.representative, aggregate_results, spec.expr.get()));
                }
                sorted_groups.push_back(std::move(gsk));
            }

            if (!query.order_by.empty()) {
                auto comp = [&query](const GroupSortKey& a, const GroupSortKey& b) {
                    for (size_t i = 0; i < query.order_by.size(); ++i) {
                        int cmp = compare_gql_values(a.sort_keys[i], b.sort_keys[i]);
                        if (cmp != 0) {
                            return query.order_by[i].ascending ? (cmp < 0) : (cmp > 0);
                        }
                    }
                    return false;
                };
                
                const auto window = page_window(query);
                if (window && *window < sorted_groups.size()) {
                    std::partial_sort(sorted_groups.begin(), sorted_groups.begin() + static_cast<std::ptrdiff_t>(*window), sorted_groups.end(), comp);
                    sorted_groups.resize(static_cast<size_t>(*window));
                } else {
                    std::stable_sort(sorted_groups.begin(), sorted_groups.end(), comp);
                }
            }

            if (query_ptr->profile) {
                auto agg_end = std::chrono::steady_clock::now();
                auto node = query_ptr->plan_nodes["aggregate"];
                if (node) {
                    node->actual_rows = sorted_groups.size();
                    node->time_ms = std::chrono::duration<double, std::milli>(agg_end - agg_start).count();
                }
            }

            for (const auto& gsk : sorted_groups) {
                if (query.distinct) {
                    std::string serialized_distinct;
                    for (const auto& v : gsk.projected_row) {
                        serialized_distinct += serialize_gql_value(v) + ",";
                    }
                    if (seen_distinct.insert(serialized_distinct).second) {
                        query_res.rows.push_back(gsk.projected_row);
                    }
                } else {
                    query_res.rows.push_back(gsk.projected_row);
                }
            }
        } else {
            if (!query.order_by.empty() && !query_res.is_sorted) {
                auto sort_start = std::chrono::steady_clock::now();
                std::vector<RowSortKey> sorted_keys;
                for (auto& row : filtered_rows) {
                    RowSortKey rk;
                    rk.row = row;
                    for (const auto& spec : query.order_by) {
                        rk.keys.push_back(evaluate_expression(row, spec.expr.get()));
                    }
                    sorted_keys.push_back(std::move(rk));
                }

                auto comp = [&query](const RowSortKey& a, const RowSortKey& b) {
                    for (size_t i = 0; i < query.order_by.size(); ++i) {
                        int cmp = compare_gql_values(a.keys[i], b.keys[i]);
                        if (cmp != 0) {
                            return query.order_by[i].ascending ? (cmp < 0) : (cmp > 0);
                        }
                    }
                    return false;
                };

                const auto window = page_window(query);
                if (window && *window < sorted_keys.size()) {
                    std::partial_sort(sorted_keys.begin(), sorted_keys.begin() + static_cast<std::ptrdiff_t>(*window), sorted_keys.end(), comp);
                    sorted_keys.resize(static_cast<size_t>(*window));
                } else {
                    std::stable_sort(sorted_keys.begin(), sorted_keys.end(), comp);
                }

                filtered_rows.clear();
                for (auto& rk : sorted_keys) {
                    filtered_rows.push_back(std::move(rk.row));
                }

                if (query_ptr->profile) {
                    auto sort_end = std::chrono::steady_clock::now();
                    auto node = query_ptr->plan_nodes["sort"];
                    if (node) {
                        node->actual_rows = filtered_rows.size();
                        node->time_ms = std::chrono::duration<double, std::milli>(sort_end - sort_start).count();
                    }
                }
            } else if (page_window(query) && !query.distinct && *page_window(query) < filtered_rows.size()) {
                // DISTINCT collapses rows during projection, so the raw matched rows cannot be
                // truncated to the LIMIT here; the post-dedup resize below enforces it instead. The cut is
                // to offset+limit, not limit: an OFFSET skips past rows that are still needed.
                auto limit_start = std::chrono::steady_clock::now();
                filtered_rows.resize(static_cast<size_t>(*page_window(query)));
                if (query_ptr->profile) {
                    auto limit_end = std::chrono::steady_clock::now();
                    auto node = query_ptr->plan_nodes["limit"];
                    if (node) {
                        node->actual_rows = filtered_rows.size();
                        node->time_ms = std::chrono::duration<double, std::milli>(limit_end - limit_start).count();
                    }
                }
            }

            auto produce_start = std::chrono::steady_clock::now();
            for (const auto& row : filtered_rows) {
                std::vector<GqlValue> projected;
                for (size_t i = 0; i < query.returns.size(); ++i) {
                    const auto& item = query.returns[i];
                    projected.push_back(evaluate_expression(row, item.expr.get()));
                }

                if (query.distinct) {
                    std::string serialized_distinct;
                    for (const auto& v : projected) {
                        serialized_distinct += serialize_gql_value(v) + ",";
                    }
                    if (seen_distinct.insert(serialized_distinct).second) {
                        query_res.rows.push_back(std::move(projected));
                    }
                } else {
                    query_res.rows.push_back(std::move(projected));
                }
            }

            if (query_ptr->profile) {
                auto produce_end = std::chrono::steady_clock::now();
                auto node = query_ptr->plan_nodes["produce_results"];
                if (node) {
                    node->actual_rows = query_res.rows.size();
                    node->time_ms = std::chrono::duration<double, std::milli>(produce_end - produce_start).count();
                }
                if (query.distinct) {
                    auto dist_node = query_ptr->plan_nodes["distinct"];
                    if (dist_node) {
                        dist_node->actual_rows = query_res.rows.size();
                        dist_node->time_ms = std::chrono::duration<double, std::milli>(produce_end - produce_start).count();
                    }
                }
            }
        }

        apply_page(query_res.rows, query);
        query_res.is_paged = true;

        return seastar::make_ready_future<QueryResult>(std::move(query_res));
    });
}
/**
 * @brief Validates database constraints after executing queries containing write operations.
 * @param graph The database graph instance.
 * @return A future resolving when validation completes successfully or throws on violation.
 */
static seastar::future<> validate_constraints(ragedb::Graph& graph) {
    const auto& constraints = GqlVirtualCatalog::local().get_constraints();
    if (constraints.empty()) {
        return seastar::make_ready_future<>();
    }
    
    seastar::future<> f = seastar::make_ready_future<>();
    for (const auto& [name, query_str] : constraints) {
        f = f.then([&graph, name = name, query_str = query_str] {
            try {
                auto constraint_query = GqlParser::parse(query_str);
                constraint_query.limit = 1;
                auto query_ptr = std::make_shared<GqlQuery>(std::move(constraint_query));
                return execute_query_internal(graph, query_ptr)
                .then([name](QueryResult res) {
                    if (!res.rows.empty()) {
                        throw std::runtime_error("Constraint violation: '" + name + "' failed");
                    }
                });
            } catch (...) {
                return seastar::make_exception_future<>(std::current_exception());
            }
        });
    }
    return f;
}

/**
 * @brief Executes a pre-parsed and typechecked GqlQuery.
 * @param graph The database graph instance.
 * @param query_val The GqlQuery object.
 * @return A future resolving to the query result formatted as a JSON string.
 */
seastar::future<std::string> GqlExecutor::execute(ragedb::Graph& graph, GqlQuery query_val) {
    GqlTypechecker::typecheck(graph, query_val);

    if (query_val.explain) {
        auto plan = build_query_plan(graph, query_val);
        std::vector<std::vector<GqlValue>> plan_rows;
        flatten_plan_tree(plan, plan_rows, "", true);

        std::vector<std::string> column_names = { "Operator", "Details", "Outputs", "Cache" };

        std::string json_res = "[";
        bool first_row = true;
        for (const auto& row : plan_rows) {
            if (!first_row) json_res += ", ";
            json_res += "{";
            bool first_col = true;
            for (size_t i = 0; i < column_names.size() - 1; ++i) {
                if (!first_col) json_res += ", ";
                json_res += "\"" + column_names[i] + "\": " + serialize_gql_value(row[i]);
                first_col = false;
            }
            std::string cache_status = query_val.plan_cache_hit ? "\"HIT\"" : "\"MISS\"";
            json_res += ", \"Cache\": " + cache_status;
            json_res += "}";
            first_row = false;
        }
        json_res += "]";

        return seastar::make_ready_future<std::string>(json_res);
    }

    if (query_val.clear_cache) {
        return graph.shard.invoke_on_all([](Shard&) {
            GqlQueryCache::local().clear();
            WccCache::local().clear();
            TransitiveReachabilityCache::local().clear();
        }).then([] {
            return seastar::make_ready_future<std::string>("{\"status\": \"cache cleared\"}");
        });
    }

    if (query_val.schema_op.has_value()) {
        return GqlCatalog::execute_schema_op(graph, *query_val.schema_op);
    }

    auto query_ptr = std::make_shared<GqlQuery>(std::move(query_val));

    if (query_ptr->profile) {
        query_ptr->plan_root = build_query_plan(graph, *query_ptr);
        index_plan_nodes(query_ptr->plan_root, query_ptr->plan_nodes);
    }

    return execute_query_internal(graph, query_ptr)
    .then([&graph, query_ptr](QueryResult result) {
        if (!query_ptr->writes.empty()) {
            return graph.shard.invoke_on_all([](Shard&) {
                WccCache::local().clear();
                TransitiveReachabilityCache::local().clear();
            }).then([&graph]() {
                return validate_constraints(graph);
            }).then([result = std::move(result)]() mutable {
                return result;
            });
        }
        return seastar::make_ready_future<QueryResult>(std::move(result));
    })
    .then([query_ptr](QueryResult result) {
        if (query_ptr->profile) {
            std::vector<std::vector<GqlValue>> plan_rows;
            flatten_plan_tree(query_ptr->plan_root, plan_rows, "", true);

            std::vector<std::string> column_names = { "Operator", "Details", "Outputs", "Actual Rows", "Time (ms)", "Cache" };

            std::string json_res = "[";
            bool first_row = true;
            for (const auto& row : plan_rows) {
                if (!first_row) json_res += ", ";
                json_res += "{";
                bool first_col = true;
                for (size_t i = 0; i < column_names.size() - 1; ++i) {
                    if (!first_col) json_res += ", ";
                    json_res += "\"" + column_names[i] + "\": " + serialize_gql_value(row[i]);
                    first_col = false;
                }
                std::string cache_status = query_ptr->plan_cache_hit ? "\"HIT\"" : "\"MISS\"";
                json_res += ", \"Cache\": " + cache_status;
                json_res += "}";
                first_row = false;
            }
            json_res += "]";

            return seastar::make_ready_future<std::string>(json_res);
        }

        const auto& query = *query_ptr;

        // Set operations combine un-paged branch results, so the page applies to the combination here.
        // Every other path already paged its own rows and must not be paged a second time.
        if (!result.is_paged) {
            if (!query.order_by.empty() && !result.is_sorted) {
                // Sort down to the page window, not the bare limit: the OFFSET skips past rows the sort
                // would otherwise have pruned away.
                const auto window = page_window(query);
                sort_combined_result(result, query.order_by,
                                     window ? std::optional<size_t>(static_cast<size_t>(*window)) : std::nullopt);
            }
            apply_page(result.rows, query);
        }

        std::string json_res = "[";
        bool first_row = true;
        for (const auto& row : result.rows) {
            if (!first_row) json_res += ", ";
            json_res += "{";
            bool first_col = true;
            for (size_t i = 0; i < result.column_names.size(); ++i) {
                if (!first_col) json_res += ", ";
                json_res += "\"" + result.column_names[i] + "\": " + serialize_gql_value(row[i]);
                first_col = false;
            }
            json_res += "}";
            first_row = false;
        }
        json_res += "]";

        return seastar::make_ready_future<std::string>(json_res);
    });
}
/**
 * @brief Helper utility to trim whitespace from the start and end of a string.
 * @param str The string to trim.
 * @return The trimmed string.
 */
static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}
/**
 * @brief Parses, optimizes, caches, and executes a GQL query string.
 * @param graph The database graph instance.
 * @param query_str The query string to run.
 * @return A future resolving to the query result formatted as a JSON string.
 */
seastar::future<std::string> GqlExecutor::execute(ragedb::Graph& graph, const std::string& query_str) {
    std::string key = trim(query_str);
    if (key.empty()) {
        return seastar::make_exception_future<std::string>(std::runtime_error("Empty query"));
    }

    if (auto cached_opt = GqlQueryCache::local().get(key)) {
        GqlQuery cloned = std::move(*cached_opt);
        cloned.plan_cache_hit = true;
        return execute(graph, std::move(cloned));
    }

    try {
        auto query = GqlParser::parse(key);
        GqlOptimizer::optimize(graph, query);

        if (query.schema_op.has_value()) {
            return execute(graph, std::move(query))
            .then([&graph](std::string res) {
                return graph.shard.invoke_on_all([](Shard&) {
                    GqlQueryCache::local().clear();
                }).then([res = std::move(res)] {
                    return res;
                });
            });
        }

        if (query.clear_cache) {
            return execute(graph, std::move(query));
        }

        query.plan_cache_hit = false;
        GqlQueryCache::local().put(key, query);
        return execute(graph, std::move(query));
    } catch (...) {
        return seastar::make_exception_future<std::string>(std::current_exception());
    }
}

void clear_registries() {
    GqlQueryCache::local().clear();
    GqlVirtualCatalog::local().clear();
}

} // namespace ragedb::gql
