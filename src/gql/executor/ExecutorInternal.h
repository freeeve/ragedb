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

#ifndef RAGEDB_GQL_EXECUTORINTERNAL_H
#define RAGEDB_GQL_EXECUTORINTERNAL_H

#include <vector>
#include <string>
#include <map>
#include <set>
#include <unordered_set>
#include <optional>
#include <memory>
#include <cmath>
#include <algorithm>
#include <limits>
#include <cstdint>
#include <seastar/core/future.hh>
#include "../graph/Graph.h"
#include "GqlValue.h"
#include "GqlAst.h"
#include "FactorNode.h"
#include "ProjectionPruner.h"

namespace ragedb::gql {

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
inline std::optional<uint64_t> page_window(const GqlQuery& q) {
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
void apply_page(std::vector<Row>& rows, const GqlQuery& q) {
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
inline void collect_numeric(const GqlValue& v, std::vector<double>& out) {
    if (v.type != GqlValue::PROPERTY) return;
    if (std::holds_alternative<int64_t>(v.property)) out.push_back(static_cast<double>(std::get<int64_t>(v.property)));
    else if (std::holds_alternative<double>(v.property)) out.push_back(std::get<double>(v.property));
}

inline GqlValue stddev_of(const std::vector<double>& values, bool sample) {
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

/**
 * @brief Derive the output column name for a RETURN item. The single source of truth for column
 *        naming: query_result_to_rows turns column names into next-segment bindings in WITH
 *        pipelines, so every path that names columns must agree.
 */
std::string return_item_column_name(const ReturnItem& item, size_t index);

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
    std::vector<GqlValue> collect_vals;   // COLLECT: values in row order
    std::vector<double> numeric_vals;     // STDDEV: numeric values for the two-pass stddev

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
        } else if (kind == AggregateKind::COLLECT) {
            if (v.type != GqlValue::NIL) collect_vals.push_back(std::move(v));
        } else if (kind == AggregateKind::STDDEV_POP || kind == AggregateKind::STDDEV_SAMP) {
            collect_numeric(v, numeric_vals);
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
        if (kind == AggregateKind::COLLECT) return GqlValue(collect_vals);
        if (kind == AggregateKind::STDDEV_POP || kind == AggregateKind::STDDEV_SAMP) {
            return stddev_of(numeric_vals, kind == AggregateKind::STDDEV_SAMP);
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
        if (kind == AggregateKind::COLLECT) {
            return GqlValue(std::vector<GqlValue>(distinct_vals.begin(), distinct_vals.end()));
        }
        if (kind == AggregateKind::STDDEV_POP || kind == AggregateKind::STDDEV_SAMP) {
            std::vector<double> vals;
            for (const auto& v : distinct_vals) collect_numeric(v, vals);
            return stddev_of(vals, kind == AggregateKind::STDDEV_SAMP);
        }
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
 * @brief Streaming fast-path aggregate machinery: shape detectors, eligibility gates, and the
 *        streamed runners. Defined in executor/StreamingAggregates.cpp.
 */
bool try_plan_simple_node_count(const GqlQuery& q, SimpleNodeCountPlan& out);

bool plan_streaming_edge_aggregate(const GqlQuery& q, EdgeAggPlan& out);

bool stream_eligible(const GqlQuery& q);

bool stream_group_eligible(const GqlQuery& q);

seastar::future<QueryResult> run_streaming_edge_aggregate(
        ragedb::Graph& graph, std::shared_ptr<GqlQuery> query_ptr, EdgeAggPlan plan);

seastar::future<QueryResult> run_streaming_topk(
        ragedb::Graph& graph, std::shared_ptr<GqlQuery> query_ptr,
        std::optional<std::vector<GqlRow>> incoming, const ProjectionPruner& pruner);

seastar::future<QueryResult> run_streaming_group_fold(
        ragedb::Graph& graph, std::shared_ptr<GqlQuery> query_ptr,
        std::optional<std::vector<GqlRow>> incoming, const ProjectionPruner& pruner,
        std::vector<const Expression*> grouping_keys, std::vector<const AggregateExpr*> aggs);

} // namespace ragedb::gql

#endif // RAGEDB_GQL_EXECUTORINTERNAL_H
