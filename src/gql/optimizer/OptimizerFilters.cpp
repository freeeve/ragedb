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

// The two halves of predicate pushdown: lifting the conjuncts a scan can evaluate itself out of a WHERE,
// and rebuilding what is left so the executor still applies everything that was not pushed. They are
// written as a pair because dropping a conjunct from one without accounting for it in the other silently
// widens the result. Split out of OptimizerRewrites.cpp, which keeps the analysis and count-rewrite
// helpers.

#include "OptimizerUtils.h"
#include "../GqlValue.h"
#include <limits>

namespace ragedb::gql {

void extract_filters(Expression* expr, std::map<std::string, std::vector<PropertyFilter>>& pushdowns) {
    if (!expr) return;

    if (expr->kind == ExpressionKind::BINARY_OP) {
        auto* bin = static_cast<BinaryOpExpr*>(expr);
        if (bin->op == BinaryOpKind::AND) {
            extract_filters(bin->left.get(), pushdowns);
            extract_filters(bin->right.get(), pushdowns);
        } else {
            Operation op = Operation::UNKNOWN;
            switch (bin->op) {
                case BinaryOpKind::EQ: op = Operation::EQ; break;
                case BinaryOpKind::NE: op = Operation::NEQ; break;
                case BinaryOpKind::LT: op = Operation::LT; break;
                case BinaryOpKind::LE: op = Operation::LTE; break;
                case BinaryOpKind::GT: op = Operation::GT; break;
                case BinaryOpKind::GE: op = Operation::GTE; break;
                default: break;
            }

            if (op != Operation::UNKNOWN) {
                if (bin->left->kind == ExpressionKind::PROPERTY_LOOKUP && bin->right->kind == ExpressionKind::LITERAL) {
                    auto* prop_lookup = static_cast<PropertyLookupExpr*>(bin->left.get());
                    auto* lit = static_cast<LiteralExpr*>(bin->right.get());
                    pushdowns[prop_lookup->variable].push_back({prop_lookup->property, op, lit->value});
                } else if (bin->right->kind == ExpressionKind::PROPERTY_LOOKUP && bin->left->kind == ExpressionKind::LITERAL) {
                    auto* prop_lookup = static_cast<PropertyLookupExpr*>(bin->right.get());
                    auto* lit = static_cast<LiteralExpr*>(bin->left.get());
                    Operation inverted_op = op;
                    if (op == Operation::LT) inverted_op = Operation::GT;
                    else if (op == Operation::LTE) inverted_op = Operation::GTE;
                    else if (op == Operation::GT) inverted_op = Operation::LT;
                    else if (op == Operation::GTE) inverted_op = Operation::LTE;
                    pushdowns[prop_lookup->variable].push_back({prop_lookup->property, inverted_op, lit->value});
                }
            }
        }
    }
}

std::unique_ptr<Expression> rebuild_expression_without_pushed_predicates(std::unique_ptr<Expression> expr, const std::map<std::string, std::vector<PropertyFilter>>& pushdowns) {
    if (!expr) return nullptr;

    if (expr->kind == ExpressionKind::BINARY_OP) {
        auto* bin = static_cast<BinaryOpExpr*>(expr.get());
        if (bin->op == BinaryOpKind::AND) {
            auto new_left = rebuild_expression_without_pushed_predicates(std::move(bin->left), pushdowns);
            auto new_right = rebuild_expression_without_pushed_predicates(std::move(bin->right), pushdowns);
            if (new_left && new_right) {
                return std::make_unique<BinaryOpExpr>(BinaryOpKind::AND, std::move(new_left), std::move(new_right));
            } else if (new_left) {
                return new_left;
            } else {
                return new_right;
            }
        } else {
            Operation op = Operation::UNKNOWN;
            switch (bin->op) {
                case BinaryOpKind::EQ: op = Operation::EQ; break;
                case BinaryOpKind::NE: op = Operation::NEQ; break;
                case BinaryOpKind::LT: op = Operation::LT; break;
                case BinaryOpKind::LE: op = Operation::LTE; break;
                case BinaryOpKind::GT: op = Operation::GT; break;
                case BinaryOpKind::GE: op = Operation::GTE; break;
                default: break;
            }
            if (op != Operation::UNKNOWN) {
                if (bin->left->kind == ExpressionKind::PROPERTY_LOOKUP && bin->right->kind == ExpressionKind::LITERAL) {
                    auto* prop_lookup = static_cast<PropertyLookupExpr*>(bin->left.get());
                    auto it = pushdowns.find(prop_lookup->variable);
                    if (it != pushdowns.end()) {
                        for (const auto& filter : it->second) {
                            if (filter.property == prop_lookup->property && filter.op == op) {
                                return nullptr;
                            }
                        }
                    }
                } else if (bin->right->kind == ExpressionKind::PROPERTY_LOOKUP && bin->left->kind == ExpressionKind::LITERAL) {
                    auto* prop_lookup = static_cast<PropertyLookupExpr*>(bin->right.get());
                    auto it = pushdowns.find(prop_lookup->variable);
                    if (it != pushdowns.end()) {
                        Operation inverted_op = op;
                        if (op == Operation::LT) inverted_op = Operation::GT;
                        else if (op == Operation::LTE) inverted_op = Operation::GTE;
                        else if (op == Operation::GT) inverted_op = Operation::LT;
                        else if (op == Operation::GTE) inverted_op = Operation::LTE;
                        for (const auto& filter : it->second) {
                            if (filter.property == prop_lookup->property && filter.op == inverted_op) {
                                return nullptr;
                            }
                        }
                    }
                }
            }
        }
    }
    return expr;
}

} // namespace ragedb::gql
