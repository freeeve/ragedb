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

#ifndef RAGEDB_OPTIMIZERUTILS_H
#define RAGEDB_OPTIMIZERUTILS_H

/**
 * @file OptimizerUtils.h
 * @brief Common helper structures and functions for RageDB Semantic Query Optimizer passes.
 * 
 * Provides shared abstractions such as:
 *  - Interval: A bounding interval representation used for variable constraint satisfiability checks.
 *  - VarInfo: Information gathered per variable (its label, and intervals per property).
 *  - AST Rewriters and Traversers: Functions to rewrite variable names, evaluate numeric literals,
 *    and analyze GQL query patterns for equivalence or containment.
 */

#include "../GqlAst.h"
#include <map>
#include <vector>
#include <string>
#include <set>
#include <memory>

namespace ragedb::gql {

struct Interval {
    bool has_lower = false;
    double lower_val = 0;
    bool lower_inclusive = false;

    bool has_upper = false;
    double upper_val = 0;
    bool upper_inclusive = false;

    bool is_empty() const;
    bool contains(const Interval& other) const;
    void intersect(const Interval& other);
};

struct VarInfo {
    std::string variable;
    std::string label;
    std::map<std::string, Interval> intervals;
};

// Label & Pattern Equivalence helpers
bool is_equivalent_label_expr(const std::shared_ptr<LabelExpression>& e1, const std::shared_ptr<LabelExpression>& e2);
bool is_label_subsumed(const std::shared_ptr<LabelExpression>& e1, const std::shared_ptr<LabelExpression>& e2);
bool is_equivalent_properties(const std::map<std::string, property_type_t>& p1, const std::map<std::string, property_type_t>& p2);
bool is_equivalent_pattern(const PathPattern& p1, const PathPattern& p2);

template <typename SmartPtr>
void rewrite_expr_vars(SmartPtr& expr, const std::map<std::string, std::string>& var_map);

/**
 * @brief Whether @p name is referenced anywhere in this expression.
 *
 * Passes ask this before ERASING the match that binds a variable, so the two answers are not equally
 * costly: reporting a reference that is not really there only forgoes an optimization, while missing one
 * leaves a live reference with nothing to bind it. Every arm must therefore be present, and the walk
 * deliberately errs toward reporting -- it descends into a scoped binder's body without checking whether
 * the element name shadows @p name, and into a nested pattern's own variables.
 */
inline bool expression_references_variable(const Expression* expr, const std::string& name) {
    if (!expr) return false;
    switch (expr->kind) {
        case ExpressionKind::VARIABLE:
            return static_cast<const VariableExpr*>(expr)->name == name;
        case ExpressionKind::PROPERTY_LOOKUP:
            return static_cast<const PropertyLookupExpr*>(expr)->variable == name;
        case ExpressionKind::UNARY_OP:
            return expression_references_variable(static_cast<const UnaryOpExpr*>(expr)->expr.get(), name);
        case ExpressionKind::BINARY_OP: {
            const auto* bin = static_cast<const BinaryOpExpr*>(expr);
            return expression_references_variable(bin->left.get(), name) ||
                   expression_references_variable(bin->right.get(), name);
        }
        case ExpressionKind::AGGREGATION:
            return expression_references_variable(static_cast<const AggregateExpr*>(expr)->expr.get(), name);
        case ExpressionKind::IS_NULL_CHECK:
            return expression_references_variable(static_cast<const IsNullExpr*>(expr)->expr.get(), name);
        case ExpressionKind::IS_LABELED:
            return expression_references_variable(static_cast<const IsLabeledExpr*>(expr)->value.get(), name);
        case ExpressionKind::IS_DIRECTED:
            return expression_references_variable(static_cast<const IsDirectedExpr*>(expr)->value.get(), name);
        case ExpressionKind::IS_SOURCE_DEST: {
            const auto* sd = static_cast<const IsSourceDestExpr*>(expr);
            return expression_references_variable(sd->value.get(), name) ||
                   expression_references_variable(sd->edge.get(), name);
        }
        case ExpressionKind::CAST:
            return expression_references_variable(static_cast<const CastExpr*>(expr)->value.get(), name);
        case ExpressionKind::FUNCTION_CALL: {
            for (const auto& a : static_cast<const FunctionCallExpr*>(expr)->args) {
                if (expression_references_variable(a.get(), name)) return true;
            }
            return false;
        }
        case ExpressionKind::CASE_WHEN: {
            const auto* ce = static_cast<const CaseExpr*>(expr);
            for (const auto& br : ce->branches) {
                if (expression_references_variable(br.first.get(), name)) return true;
                if (expression_references_variable(br.second.get(), name)) return true;
            }
            return expression_references_variable(ce->else_expr.get(), name);
        }
        case ExpressionKind::IN_LIST: {
            const auto* in = static_cast<const InExpr*>(expr);
            return expression_references_variable(in->value.get(), name) ||
                   expression_references_variable(in->list.get(), name);
        }
        case ExpressionKind::LIST_LITERAL: {
            for (const auto& el : static_cast<const ListExpr*>(expr)->elements) {
                if (expression_references_variable(el.get(), name)) return true;
            }
            return false;
        }
        case ExpressionKind::LIST_INDEX: {
            const auto* ix = static_cast<const IndexExpr*>(expr);
            return expression_references_variable(ix->list.get(), name) ||
                   expression_references_variable(ix->index.get(), name);
        }
        case ExpressionKind::LIST_COMPREHENSION: {
            const auto* lc = static_cast<const ListComprehensionExpr*>(expr);
            return expression_references_variable(lc->list.get(), name) ||
                   expression_references_variable(lc->filter.get(), name) ||
                   expression_references_variable(lc->projection.get(), name);
        }
        case ExpressionKind::QUANTIFIED_PREDICATE: {
            const auto* qp = static_cast<const QuantifiedPredicateExpr*>(expr);
            return expression_references_variable(qp->list.get(), name) ||
                   expression_references_variable(qp->predicate.get(), name);
        }
        case ExpressionKind::EXISTS:
        case ExpressionKind::SIZE_OP: {
            const std::vector<MatchStatement>* matches = nullptr;
            const Expression* where = nullptr;
            if (expr->kind == ExpressionKind::EXISTS) {
                const auto* ex = static_cast<const ExistsExpr*>(expr);
                matches = &ex->matches;
                where = ex->where_expr.get();
            } else {
                const auto* sz = static_cast<const SizeExpr*>(expr);
                matches = &sz->matches;
                where = sz->where_expr.get();
            }
            if (expression_references_variable(where, name)) return true;
            for (const auto& m : *matches) {
                for (const auto& node : m.pattern.nodes) {
                    if (node.variable == name) return true;
                    if (expression_references_variable(node.where_expr.get(), name)) return true;
                }
                for (const auto& edge : m.pattern.edges) {
                    if (edge.variable == name) return true;
                    if (expression_references_variable(edge.where_expr.get(), name)) return true;
                    if (expression_references_variable(edge.cost_expr.get(), name)) return true;
                }
            }
            return false;
        }
        default:
            return false;
    }
}

/**
 * @brief Whether any list comprehension or quantified predicate in this expression binds an iteration
 *        variable called @p name.
 *
 * Callers rename one variable onto another. If the destination name is also a scoped element name, the
 * rename would push an outer reference into that element's scope and silently rebind it -- so a merge is
 * declined when this returns true. Under-reporting reintroduces that capture, so this must visit the same
 * arms rewrite_expr_vars does; keep the two in step.
 */
inline bool binds_scoped_variable(const Expression* expr, const std::string& name) {
    if (!expr) return false;
    switch (expr->kind) {
        case ExpressionKind::LIST_COMPREHENSION: {
            auto* lc = static_cast<const ListComprehensionExpr*>(expr);
            return lc->variable == name || binds_scoped_variable(lc->list.get(), name) ||
                   binds_scoped_variable(lc->filter.get(), name) ||
                   binds_scoped_variable(lc->projection.get(), name);
        }
        case ExpressionKind::QUANTIFIED_PREDICATE: {
            auto* qp = static_cast<const QuantifiedPredicateExpr*>(expr);
            return qp->variable == name || binds_scoped_variable(qp->list.get(), name) ||
                   binds_scoped_variable(qp->predicate.get(), name);
        }
        case ExpressionKind::UNARY_OP:
            return binds_scoped_variable(static_cast<const UnaryOpExpr*>(expr)->expr.get(), name);
        case ExpressionKind::BINARY_OP: {
            auto* bin = static_cast<const BinaryOpExpr*>(expr);
            return binds_scoped_variable(bin->left.get(), name) ||
                   binds_scoped_variable(bin->right.get(), name);
        }
        case ExpressionKind::AGGREGATION:
            return binds_scoped_variable(static_cast<const AggregateExpr*>(expr)->expr.get(), name);
        case ExpressionKind::IS_NULL_CHECK:
            return binds_scoped_variable(static_cast<const IsNullExpr*>(expr)->expr.get(), name);
        case ExpressionKind::IS_LABELED:
            return binds_scoped_variable(static_cast<const IsLabeledExpr*>(expr)->value.get(), name);
        case ExpressionKind::IS_DIRECTED:
            return binds_scoped_variable(static_cast<const IsDirectedExpr*>(expr)->value.get(), name);
        case ExpressionKind::IS_SOURCE_DEST: {
            auto* sd = static_cast<const IsSourceDestExpr*>(expr);
            return binds_scoped_variable(sd->value.get(), name) ||
                   binds_scoped_variable(sd->edge.get(), name);
        }
        case ExpressionKind::CAST:
            return binds_scoped_variable(static_cast<const CastExpr*>(expr)->value.get(), name);
        case ExpressionKind::FUNCTION_CALL: {
            for (const auto& a : static_cast<const FunctionCallExpr*>(expr)->args) {
                if (binds_scoped_variable(a.get(), name)) return true;
            }
            return false;
        }
        case ExpressionKind::CASE_WHEN: {
            auto* ce = static_cast<const CaseExpr*>(expr);
            for (const auto& br : ce->branches) {
                if (binds_scoped_variable(br.first.get(), name)) return true;
                if (binds_scoped_variable(br.second.get(), name)) return true;
            }
            return binds_scoped_variable(ce->else_expr.get(), name);
        }
        case ExpressionKind::IN_LIST: {
            auto* in = static_cast<const InExpr*>(expr);
            return binds_scoped_variable(in->value.get(), name) ||
                   binds_scoped_variable(in->list.get(), name);
        }
        case ExpressionKind::LIST_LITERAL: {
            for (const auto& el : static_cast<const ListExpr*>(expr)->elements) {
                if (binds_scoped_variable(el.get(), name)) return true;
            }
            return false;
        }
        case ExpressionKind::LIST_INDEX: {
            auto* ix = static_cast<const IndexExpr*>(expr);
            return binds_scoped_variable(ix->list.get(), name) ||
                   binds_scoped_variable(ix->index.get(), name);
        }
        case ExpressionKind::EXISTS: {
            auto* ex = static_cast<const ExistsExpr*>(expr);
            return binds_scoped_variable(ex->where_expr.get(), name);
        }
        case ExpressionKind::SIZE_OP: {
            auto* sz = static_cast<const SizeExpr*>(expr);
            return binds_scoped_variable(sz->where_expr.get(), name);
        }
        default:
            return false;
    }
}

/**
 * @brief Whether renaming some other variable onto @p name would be captured by a scoped binder
 *        anywhere the callers rewrite: the projection, sort keys, LET bindings, writes and filters.
 */
inline bool query_binds_scoped_variable(const GqlQuery& query, const std::string& name) {
    for (const auto& item : query.returns) {
        if (binds_scoped_variable(item.expr.get(), name)) return true;
    }
    for (const auto& spec : query.order_by) {
        if (binds_scoped_variable(spec.expr.get(), name)) return true;
    }
    for (const auto& let : query.let_bindings) {
        if (binds_scoped_variable(let.expr.get(), name)) return true;
    }
    for (const auto& write : query.writes) {
        if (binds_scoped_variable(write.set_expr.get(), name)) return true;
    }
    if (binds_scoped_variable(query.where_expr.get(), name)) return true;
    for (const auto& match : query.matches) {
        for (const auto& node : match.pattern.nodes) {
            if (binds_scoped_variable(node.where_expr.get(), name)) return true;
        }
        for (const auto& edge : match.pattern.edges) {
            if (binds_scoped_variable(edge.where_expr.get(), name)) return true;
            if (binds_scoped_variable(edge.cost_expr.get(), name)) return true;
        }
    }
    return false;
}

/**
 * @brief Renames the node/edge variables a nested pattern binds, and the expressions hanging off it.
 *        Shared by the EXISTS and SIZE_OP arms, which have the same pattern-plus-WHERE shape.
 */
inline void rewrite_pattern_vars(std::vector<MatchStatement>& matches,
                                 const std::map<std::string, std::string>& var_map) {
    for (auto& match : matches) {
        for (auto& node : match.pattern.nodes) {
            auto it = var_map.find(node.variable);
            if (it != var_map.end()) node.variable = it->second;
            rewrite_expr_vars(node.where_expr, var_map);
        }
        for (auto& edge : match.pattern.edges) {
            auto it = var_map.find(edge.variable);
            if (it != var_map.end()) edge.variable = it->second;
            rewrite_expr_vars(edge.where_expr, var_map);
            rewrite_expr_vars(edge.cost_expr, var_map);
        }
    }
}

/**
 * @brief Renames variable references throughout an expression tree.
 *
 * Callers use this while MERGING two variables and then dropping the match that bound the discarded
 * one, so an arm this walk fails to descend into leaves a reference with nothing left to bind it.
 * Every ExpressionKind that owns child expressions must therefore appear below -- when a new kind is
 * added to the AST, add it here too.
 *
 * Kinds binding their own scoped iteration variable (list comprehension, quantified predicate) stop the
 * descent when that variable shadows a name being renamed: inside the body the name means the element.
 */
template <typename SmartPtr>
void rewrite_expr_vars(SmartPtr& expr, const std::map<std::string, std::string>& var_map) {
    if (!expr) return;
    if (expr->kind == ExpressionKind::VARIABLE) {
        auto* ve = static_cast<VariableExpr*>(expr.get());
        auto it = var_map.find(ve->name);
        if (it != var_map.end()) {
            ve->name = it->second;
        }
    } else if (expr->kind == ExpressionKind::PROPERTY_LOOKUP) {
        auto* pl = static_cast<PropertyLookupExpr*>(expr.get());
        auto it = var_map.find(pl->variable);
        if (it != var_map.end()) {
            pl->variable = it->second;
        }
    } else if (expr->kind == ExpressionKind::UNARY_OP) {
        auto* un = static_cast<UnaryOpExpr*>(expr.get());
        rewrite_expr_vars(un->expr, var_map);
    } else if (expr->kind == ExpressionKind::BINARY_OP) {
        auto* bin = static_cast<BinaryOpExpr*>(expr.get());
        rewrite_expr_vars(bin->left, var_map);
        rewrite_expr_vars(bin->right, var_map);
    } else if (expr->kind == ExpressionKind::AGGREGATION) {
        auto* agg = static_cast<AggregateExpr*>(expr.get());
        rewrite_expr_vars(agg->expr, var_map);
    } else if (expr->kind == ExpressionKind::EXISTS) {
        auto* exists = static_cast<ExistsExpr*>(expr.get());
        rewrite_expr_vars(exists->where_expr, var_map);
        rewrite_pattern_vars(exists->matches, var_map);
    } else if (expr->kind == ExpressionKind::SIZE_OP) {
        // Same shape as EXISTS: a pattern plus an optional WHERE.
        auto* size = static_cast<SizeExpr*>(expr.get());
        rewrite_expr_vars(size->where_expr, var_map);
        rewrite_pattern_vars(size->matches, var_map);
    } else if (expr->kind == ExpressionKind::IS_NULL_CHECK) {
        rewrite_expr_vars(static_cast<IsNullExpr*>(expr.get())->expr, var_map);
    } else if (expr->kind == ExpressionKind::IS_LABELED) {
        rewrite_expr_vars(static_cast<IsLabeledExpr*>(expr.get())->value, var_map);
    } else if (expr->kind == ExpressionKind::IS_DIRECTED) {
        rewrite_expr_vars(static_cast<IsDirectedExpr*>(expr.get())->value, var_map);
    } else if (expr->kind == ExpressionKind::IS_SOURCE_DEST) {
        auto* sd = static_cast<IsSourceDestExpr*>(expr.get());
        rewrite_expr_vars(sd->value, var_map);
        rewrite_expr_vars(sd->edge, var_map);
    } else if (expr->kind == ExpressionKind::CAST) {
        rewrite_expr_vars(static_cast<CastExpr*>(expr.get())->value, var_map);
    } else if (expr->kind == ExpressionKind::FUNCTION_CALL) {
        for (auto& arg : static_cast<FunctionCallExpr*>(expr.get())->args) {
            rewrite_expr_vars(arg, var_map);
        }
    } else if (expr->kind == ExpressionKind::CASE_WHEN) {
        auto* ce = static_cast<CaseExpr*>(expr.get());
        for (auto& branch : ce->branches) {
            rewrite_expr_vars(branch.first, var_map);
            rewrite_expr_vars(branch.second, var_map);
        }
        rewrite_expr_vars(ce->else_expr, var_map);
    } else if (expr->kind == ExpressionKind::IN_LIST) {
        auto* in = static_cast<InExpr*>(expr.get());
        rewrite_expr_vars(in->value, var_map);
        rewrite_expr_vars(in->list, var_map);
    } else if (expr->kind == ExpressionKind::LIST_LITERAL) {
        for (auto& el : static_cast<ListExpr*>(expr.get())->elements) {
            rewrite_expr_vars(el, var_map);
        }
    } else if (expr->kind == ExpressionKind::LIST_INDEX) {
        auto* ix = static_cast<IndexExpr*>(expr.get());
        rewrite_expr_vars(ix->list, var_map);
        rewrite_expr_vars(ix->index, var_map);
    } else if (expr->kind == ExpressionKind::LIST_COMPREHENSION) {
        auto* lc = static_cast<ListComprehensionExpr*>(expr.get());
        rewrite_expr_vars(lc->list, var_map);
        // The iteration variable is scoped to the body. If it shadows a name being renamed, the body's
        // references belong to the element, not to the outer variable, so they must be left alone.
        if (!var_map.count(lc->variable)) {
            rewrite_expr_vars(lc->filter, var_map);
            rewrite_expr_vars(lc->projection, var_map);
        }
    } else if (expr->kind == ExpressionKind::QUANTIFIED_PREDICATE) {
        auto* qp = static_cast<QuantifiedPredicateExpr*>(expr.get());
        rewrite_expr_vars(qp->list, var_map);
        if (!var_map.count(qp->variable)) {
            rewrite_expr_vars(qp->predicate, var_map);
        }
    }
}

// Numeric & expression evaluation helpers
bool get_numeric_value(const Expression* expr, double& val);
void extract_intervals_from_expr(const Expression* expr, const std::string& target_var, std::map<std::string, Interval>& intervals, bool negate = false);
std::vector<VarInfo> collect_query_vars(const GqlQuery& query);
std::vector<VarInfo> collect_all_query_vars(const GqlQuery& query);

bool is_variable_referenced_outside_count(const Expression* expr, const std::string& var_name);
bool expression_has_distinct_aggregate(const Expression* expr);
bool query_has_distinct_aggregate(const GqlQuery& query);
bool has_post_scan_residual_predicate(const GqlQuery& query);
std::optional<uint64_t> output_row_cap(const GqlQuery& query);
bool is_simple_unbounded_right_hop(const MatchStatement& match, const PatternEdge& edge);

/**
 * @brief Selectivity estimation classes based on query predicates.
 *        Lower value = higher selectivity (fewer rows expected).
 */
enum class SelectivityClass {
    UNIQUE = 1,   ///< Filtered by a unique identifier equality (e.g. id == 1)
    INDEXED = 2,  ///< Filtered by other property values (which can use a secondary index)
    SCAN = 3      ///< No filter bounds, requires scanning the whole node label
};

SelectivityClass estimate_selectivity(const std::string& var_name, const std::vector<VarInfo>& q_vars);
bool reverse_match_pattern_if_safe(MatchStatement& match);
void rewrite_count_to_sum_degree(std::unique_ptr<Expression>& expr, const std::string& start_var, const std::string& end_var, const std::string& edge_var, const std::string& degree_prop, bool& rewritten);
void extract_rel_types(const LabelExpression* expr, std::vector<std::string>& rel_types);
void rewrite_khop_count_to_var(std::unique_ptr<Expression>& expr, const std::string& var_name);
void collect_variables_from_matches(const std::vector<MatchStatement>& matches, std::set<std::string>& vars);
void extract_filters(Expression* expr, std::map<std::string, std::vector<PropertyFilter>>& pushdowns);
std::unique_ptr<Expression> rebuild_expression_without_pushed_predicates(std::unique_ptr<Expression> expr, const std::map<std::string, std::vector<PropertyFilter>>& pushdowns);

} // namespace ragedb::gql

#endif // RAGEDB_OPTIMIZERUTILS_H
