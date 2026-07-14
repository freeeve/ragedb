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

#include "GqlValue.h"
#include "../graph/types/Date.h"
#include <seastar/json/json_elements.hh>
#include <algorithm>
#include <cmath>
#include <sstream>

namespace ragedb::gql {

/**
 * @brief Compare two property_type_t variants.
 * 
 * Supports comparison between identical variant types (int64_t, double, std::string, bool)
 * and cross-type numerical comparisons between int64_t and double.
 * 
 * @param lhs Left property variant.
 * @param rhs Right property variant.
 * @return int -1 if lhs < rhs, 1 if lhs > rhs, 0 if equal or incompatible.
 */
int compare_properties(const property_type_t& lhs, const property_type_t& rhs) {
    if (lhs.index() == 0 && rhs.index() != 0) {
        return -1;
    }
    if (lhs.index() != 0 && rhs.index() == 0) {
        return 1;
    }
    if (lhs.index() == 0 && rhs.index() == 0) {
        return 0;
    }

    if (lhs.index() == rhs.index()) {
        if (std::holds_alternative<int64_t>(lhs)) {
            int64_t l = std::get<int64_t>(lhs);
            int64_t r = std::get<int64_t>(rhs);
            return (l < r) ? -1 : ((l > r) ? 1 : 0);
        }
        if (std::holds_alternative<double>(lhs)) {
            double l = std::get<double>(lhs);
            double r = std::get<double>(rhs);
            return (l < r) ? -1 : ((l > r) ? 1 : 0);
        }
        if (std::holds_alternative<std::string>(lhs)) {
            const auto& l = std::get<std::string>(lhs);
            const auto& r = std::get<std::string>(rhs);
            return (l < r) ? -1 : ((l > r) ? 1 : 0);
        }
        if (std::holds_alternative<bool>(lhs)) {
            bool l = std::get<bool>(lhs);
            bool r = std::get<bool>(rhs);
            return (l < r) ? -1 : ((l > r) ? 1 : 0);
        }
    } else {
        // Handle cross-type numerical comparison
        if (std::holds_alternative<int64_t>(lhs) && std::holds_alternative<double>(rhs)) {
            double l = static_cast<double>(std::get<int64_t>(lhs));
            double r = std::get<double>(rhs);
            return (l < r) ? -1 : ((l > r) ? 1 : 0);
        }
        if (std::holds_alternative<double>(lhs) && std::holds_alternative<int64_t>(rhs)) {
            double l = std::get<double>(lhs);
            double r = static_cast<double>(std::get<int64_t>(rhs));
            return (l < r) ? -1 : ((l > r) ? 1 : 0);
        }
    }
    return 0;
}

/**
 * @brief Compare two GqlValues.
 * 
 * Compares by GqlValue::Type first. For properties, compares using compare_properties.
 * For nodes and relationships, compares using their underlying graph ID.
 * 
 * @param a First GqlValue.
 * @param b Second GqlValue.
 * @return int -1 if a < b, 1 if a > b, 0 if equal.
 */
int compare_gql_values(const GqlValue& a, const GqlValue& b) {
    if (a.type != b.type) return (a.type < b.type) ? -1 : 1;
    if (a.type == GqlValue::PROPERTY) {
        return compare_properties(a.property, b.property);
    }
    if (a.type == GqlValue::NODE) {
        uint64_t la = a.node->getId();
        uint64_t lb = b.node->getId();
        return (la < lb) ? -1 : ((la > lb) ? 1 : 0);
    }
    if (a.type == GqlValue::RELATIONSHIP) {
        uint64_t la = a.relationship->getId();
        uint64_t lb = b.relationship->getId();
        return (la < lb) ? -1 : ((la > lb) ? 1 : 0);
    }
    if (a.type == GqlValue::RELATIONSHIP_LIST) {
        if (a.relationship_list->size() != b.relationship_list->size()) {
            return (a.relationship_list->size() < b.relationship_list->size()) ? -1 : 1;
        }
        for (size_t i = 0; i < a.relationship_list->size(); ++i) {
            uint64_t la = (*a.relationship_list)[i].getId();
            uint64_t lb = (*b.relationship_list)[i].getId();
            if (la != lb) {
                return (la < lb) ? -1 : 1;
            }
        }
        return 0;
    }
    // Compare two Path values.
    // Paths are compared first by length. If lengths match, they are compared node-by-node by ID,
    // and then relationship-by-relationship by ID.
    if (a.type == GqlValue::PATH) {
        if (a.path->length() != b.path->length()) {
            return (a.path->length() < b.path->length()) ? -1 : 1;
        }
        const auto& a_nodes = a.path->GetNodes();
        const auto& b_nodes = b.path->GetNodes();
        for (size_t i = 0; i < a_nodes.size(); ++i) {
            if (a_nodes[i].getId() != b_nodes[i].getId()) {
                return (a_nodes[i].getId() < b_nodes[i].getId()) ? -1 : 1;
            }
        }
        const auto& a_rels = a.path->GetRelationships();
        const auto& b_rels = b.path->GetRelationships();
        for (size_t i = 0; i < a_rels.size(); ++i) {
            if (a_rels[i].getId() != b_rels[i].getId()) {
                return (a_rels[i].getId() < b_rels[i].getId()) ? -1 : 1;
            }
        }
        return 0;
    }
    // Compare two list values by length, then element by element.
    if (a.type == GqlValue::LIST) {
        const auto& al = *a.list;
        const auto& bl = *b.list;
        if (al.size() != bl.size()) return (al.size() < bl.size()) ? -1 : 1;
        for (size_t i = 0; i < al.size(); ++i) {
            int c = compare_gql_values(al[i], bl[i]);
            if (c != 0) return c;
        }
        return 0;
    }
    return 0;
}

/**
 * @brief Checks if a map of target properties matches a GQL pattern's properties.
 * 
 * Matches successfully if target contains all pattern keys, with equal values.
 * 
 * @param target The map of actual node/edge properties.
 * @param pattern The map of properties specified in the GQL query pattern.
 * @return true If the target matches the pattern.
 * @return false Otherwise.
 */
bool matches_properties(const std::map<std::string, property_type_t>& target, const std::map<std::string, property_type_t>& pattern) {
    for (const auto& [key, val] : pattern) {
        auto it = target.find(key);
        if (it == target.end()) return false;
        if (compare_properties(it->second, val) != 0) return false;
    }
    return true;
}

bool matches_filters(const std::map<std::string, property_type_t>& target, const std::vector<PropertyFilter>& filters) {
    for (const auto& filter : filters) {
        auto it = target.find(filter.property);
        if (it == target.end()) return false;

        int cmp = compare_properties(it->second, filter.value);
        switch (filter.op) {
            case Operation::EQ:
                if (cmp != 0) return false;
                break;
            case Operation::NEQ:
                if (cmp == 0) return false;
                break;
            case Operation::LT:
                if (cmp >= 0) return false;
                break;
            case Operation::LTE:
                if (cmp > 0) return false;
                break;
            case Operation::GT:
                if (cmp <= 0) return false;
                break;
            case Operation::GTE:
                if (cmp < 0) return false;
                break;
            default:
                return false;
        }
    }
    return true;
}

/**
 * @brief Evaluates an AST expression node against a query row's bindings.
 * 
 * Evaluates variables, literals, properties, unary operations (NOT, NEG),
 * and binary operations (AND, OR, logical comparisons, arithmetic additions/subtractions).
 * 
 * @param row The row representing the query context and bindings.
 * @param expr The AST expression node to evaluate.
 * @return GqlValue The result of the evaluation.
 */
GqlValue evaluate_scalar_function(const GqlRow& row, const FunctionCallExpr* fc) {
    if (!fc) return GqlValue();
    // length(path | relationship-list): number of relationships.
    if (fc->name == "length") {
        if (fc->args.size() != 1) return GqlValue();
        GqlValue arg = evaluate_expression(row, fc->args[0].get());
        if (arg.type == GqlValue::PATH) {
            return GqlValue(static_cast<int64_t>(arg.path->length()));
        }
        if (arg.type == GqlValue::RELATIONSHIP_LIST) {
            return GqlValue(static_cast<int64_t>(arg.relationship_list->size()));
        }
        return GqlValue();
    }
    // zoned_datetime / datetime / date (string): epoch MILLISECONDS as int64 (LDBC canonical unit).
    // Date::convert yields epoch seconds (double), so scale by 1000. Date::fromString only accepts the
    // 'YYYY-MM-DDThh:mm:ss' form (it returns null/0 for a date-only or space-separated string), so
    // normalise: turn a ' ' separator into 'T' and pad a bare date to midnight.
    if (fc->name == "zoned_datetime" || fc->name == "datetime" || fc->name == "date" ||
        fc->name == "localdatetime") {
        if (fc->args.size() != 1) return GqlValue();
        GqlValue arg = evaluate_expression(row, fc->args[0].get());
        if (arg.type == GqlValue::PROPERTY && std::holds_alternative<std::string>(arg.property)) {
            std::string s = std::get<std::string>(arg.property);
            if (s.find('T') == std::string::npos) {
                auto sp = s.find(' ');
                if (sp != std::string::npos) s[sp] = 'T';
                else s += "T00:00:00";
            }
            double seconds = Date::convert(s);
            return GqlValue(static_cast<int64_t>(std::llround(seconds * 1000.0)));
        }
        return GqlValue();
    }
    return GqlValue(); // unknown function
}

bool matches_label_expr(const std::string& actual_type, const std::shared_ptr<LabelExpression>& expr) {
    if (!expr) return true;
    switch (expr->kind) {
        case LabelExprKind::LITERAL:
            return actual_type == expr->name;
        case LabelExprKind::NOT:
            return !matches_label_expr(actual_type, expr->expr);
        case LabelExprKind::AND:
            return matches_label_expr(actual_type, expr->left) && matches_label_expr(actual_type, expr->right);
        case LabelExprKind::OR:
            return matches_label_expr(actual_type, expr->left) || matches_label_expr(actual_type, expr->right);
        case LabelExprKind::WILDCARD:
            return !actual_type.empty() && actual_type != "_default" && actual_type != "_";
    }
    return false;
}

GqlValue apply_cast(const GqlValue& value, CastType target) {
    // Only primitive values convert; a node, relationship, path or list has no scalar representation.
    if (value.type != GqlValue::PROPERTY || std::holds_alternative<std::monostate>(value.property)) {
        return GqlValue();
    }
    const property_type_t& p = value.property;

    switch (target) {
        case CastType::STRING: {
            if (std::holds_alternative<std::string>(p)) return value;
            if (std::holds_alternative<int64_t>(p)) return GqlValue(std::to_string(std::get<int64_t>(p)));
            if (std::holds_alternative<double>(p)) {
                std::ostringstream oss;
                oss << std::get<double>(p);
                return GqlValue(oss.str());
            }
            if (std::holds_alternative<bool>(p)) return GqlValue(std::string(std::get<bool>(p) ? "true" : "false"));
            return GqlValue();
        }
        case CastType::INTEGER: {
            if (std::holds_alternative<int64_t>(p)) return value;
            if (std::holds_alternative<bool>(p)) return GqlValue(static_cast<int64_t>(std::get<bool>(p) ? 1 : 0));
            if (std::holds_alternative<double>(p)) {
                const double d = std::get<double>(p);
                if (!std::isfinite(d)) return GqlValue();
                return GqlValue(static_cast<int64_t>(std::llround(d)));
            }
            if (std::holds_alternative<std::string>(p)) {
                // A string that is not wholly an integer has no integer value, so it casts to NULL rather
                // than to the prefix it happens to start with.
                const std::string& s = std::get<std::string>(p);
                try {
                    size_t consumed = 0;
                    const long long parsed = std::stoll(s, &consumed);
                    if (consumed != s.size()) return GqlValue();
                    return GqlValue(static_cast<int64_t>(parsed));
                } catch (...) {
                    return GqlValue();
                }
            }
            return GqlValue();
        }
        case CastType::FLOAT: {
            if (std::holds_alternative<double>(p)) return value;
            if (std::holds_alternative<int64_t>(p)) return GqlValue(static_cast<double>(std::get<int64_t>(p)));
            if (std::holds_alternative<bool>(p)) return GqlValue(std::get<bool>(p) ? 1.0 : 0.0);
            if (std::holds_alternative<std::string>(p)) {
                const std::string& s = std::get<std::string>(p);
                try {
                    size_t consumed = 0;
                    const double parsed = std::stod(s, &consumed);
                    if (consumed != s.size()) return GqlValue();
                    return GqlValue(parsed);
                } catch (...) {
                    return GqlValue();
                }
            }
            return GqlValue();
        }
        case CastType::BOOLEAN: {
            if (std::holds_alternative<bool>(p)) return value;
            if (std::holds_alternative<int64_t>(p)) return GqlValue(std::get<int64_t>(p) != 0);
            if (std::holds_alternative<double>(p)) return GqlValue(std::get<double>(p) != 0.0);
            if (std::holds_alternative<std::string>(p)) {
                std::string s = std::get<std::string>(p);
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
                if (s == "true") return GqlValue(true);
                if (s == "false") return GqlValue(false);
                return GqlValue();
            }
            return GqlValue();
        }
    }
    return GqlValue();
}

GqlValue apply_is_labeled(const GqlValue& value, const std::shared_ptr<LabelExpression>& label_expr, bool negated) {
    std::string type;
    if (value.type == GqlValue::NODE) {
        type = value.node->getType();
    } else if (value.type == GqlValue::RELATIONSHIP) {
        type = value.relationship->getType();
    } else {
        // Not an entity (or unbound): unknown, which propagates rather than reading as a false match.
        return GqlValue();
    }

    const bool labeled = matches_label_expr(type, label_expr);
    return GqlValue(negated ? !labeled : labeled);
}

std::optional<std::vector<GqlValue>> as_list_elements(const GqlValue& value) {
    if (value.type == GqlValue::LIST) {
        if (!value.list) return std::vector<GqlValue>{};
        return *value.list;
    }
    if (value.type == GqlValue::RELATIONSHIP_LIST) {
        std::vector<GqlValue> elements;
        if (value.relationship_list) {
            elements.reserve(value.relationship_list->size());
            for (const auto& rel : *value.relationship_list) {
                elements.push_back(GqlValue(rel));
            }
        }
        return elements;
    }
    if (value.type == GqlValue::PROPERTY) {
        // A stored list property (e.g. a string list) is a list too, even though it arrives as a variant
        // alternative rather than as the LIST type.
        std::vector<GqlValue> elements;
        if (std::holds_alternative<std::vector<bool>>(value.property)) {
            for (bool b : std::get<std::vector<bool>>(value.property)) elements.push_back(GqlValue(b));
            return elements;
        }
        if (std::holds_alternative<std::vector<int64_t>>(value.property)) {
            for (int64_t i : std::get<std::vector<int64_t>>(value.property)) elements.push_back(GqlValue(i));
            return elements;
        }
        if (std::holds_alternative<std::vector<double>>(value.property)) {
            for (double d : std::get<std::vector<double>>(value.property)) elements.push_back(GqlValue(d));
            return elements;
        }
        if (std::holds_alternative<std::vector<std::string>>(value.property)) {
            for (const auto& s : std::get<std::vector<std::string>>(value.property)) elements.push_back(GqlValue(s));
            return elements;
        }
    }
    return std::nullopt;
}

GqlValue evaluate_expression(const GqlRow& row, const Expression* expr) {
    if (!expr) return GqlValue();

    switch (expr->kind) {
        case ExpressionKind::IS_NULL_CHECK: {
            auto* is_null_expr = static_cast<const IsNullExpr*>(expr);
            auto val = evaluate_expression(row, is_null_expr->expr.get());
            bool is_nil = (val.type == GqlValue::NIL);
            if (val.type == GqlValue::PROPERTY) {
                if (std::holds_alternative<std::monostate>(val.property)) {
                    is_nil = true;
                }
            }
            return GqlValue(is_null_expr->is_not ? !is_nil : is_nil);
        }
        case ExpressionKind::SIZE_OP: {
            return GqlValue(); // Evaluated via rewritten degree properties or subquery paths.
        }
        case ExpressionKind::AGGREGATION: {
            return GqlValue(); // Aggregations are not evaluated on single rows
        }
        case ExpressionKind::CASE_WHEN: {
            auto* ce = static_cast<const CaseExpr*>(expr);
            for (const auto& branch : ce->branches) {
                if (evaluate_expression(row, branch.first.get()).is_truthy()) {
                    return evaluate_expression(row, branch.second.get());
                }
            }
            if (ce->else_expr) {
                return evaluate_expression(row, ce->else_expr.get());
            }
            return GqlValue();
        }
        case ExpressionKind::FUNCTION_CALL: {
            return evaluate_scalar_function(row, static_cast<const FunctionCallExpr*>(expr));
        }
        case ExpressionKind::LIST_LITERAL: {
            auto* le = static_cast<const ListExpr*>(expr);
            auto items = std::make_shared<std::vector<GqlValue>>();
            items->reserve(le->elements.size());
            for (const auto& element : le->elements) {
                items->push_back(evaluate_expression(row, element.get()));
            }
            GqlValue out;
            out.type = GqlValue::LIST;
            out.list = std::move(items);
            return out;
        }
        case ExpressionKind::CAST: {
            auto* c = static_cast<const CastExpr*>(expr);
            return apply_cast(evaluate_expression(row, c->value.get()), c->target);
        }
        case ExpressionKind::IS_LABELED: {
            auto* l = static_cast<const IsLabeledExpr*>(expr);
            return apply_is_labeled(evaluate_expression(row, l->value.get()), l->label_expr, l->negated);
        }
        case ExpressionKind::IN_LIST: {
            auto* in = static_cast<const InExpr*>(expr);
            GqlValue needle = evaluate_expression(row, in->value.get());
            GqlValue hay = evaluate_expression(row, in->list.get());
            if (needle.type == GqlValue::NIL || hay.type != GqlValue::LIST) {
                return GqlValue();
            }
            for (const auto& item : *hay.list) {
                if (compare_gql_values(needle, item) == 0) {
                    return GqlValue(true);
                }
            }
            return GqlValue(false);
        }
        case ExpressionKind::EXISTS: {
            auto* exists = static_cast<const ExistsExpr*>(expr);
            if (!exists->target_variable.empty()) {
                auto it = row.bindings.find(exists->target_variable);
                if (it != row.bindings.end() && it->second.type != GqlValue::NIL) {
                    if (exists->where_expr) {
                        return evaluate_expression(row, exists->where_expr.get());
                    }
                    return GqlValue(true);
                }
                return GqlValue(false);
            }
            return GqlValue(false);
        }
        case ExpressionKind::LITERAL: {
            auto* lit = static_cast<const LiteralExpr*>(expr);
            return GqlValue(lit->value);
        }
        case ExpressionKind::VARIABLE: {
            auto* var = static_cast<const VariableExpr*>(expr);
            auto it = row.bindings.find(var->name);
            if (it != row.bindings.end()) {
                return it->second;
            }
            return GqlValue();
        }
        case ExpressionKind::PROPERTY_LOOKUP: {
            auto* prop_lookup = static_cast<const PropertyLookupExpr*>(expr);
            auto it = row.bindings.find(prop_lookup->variable);
            if (it != row.bindings.end()) {
                const auto& val = it->second;
                if (val.type == GqlValue::NODE) {
                    return GqlValue(val.node->getProperty(prop_lookup->property));
                } else if (val.type == GqlValue::RELATIONSHIP) {
                    return GqlValue(val.relationship->getProperty(prop_lookup->property));
                }
            }
            return GqlValue();
        }
        case ExpressionKind::UNARY_OP: {
            auto* un = static_cast<const UnaryOpExpr*>(expr);
            auto val = evaluate_expression(row, un->expr.get());
            if (un->op == UnaryOpKind::NOT) {
                return GqlValue(!val.is_truthy());
            } else if (un->op == UnaryOpKind::NEG) {
                if (val.type == GqlValue::PROPERTY) {
                    if (std::holds_alternative<int64_t>(val.property)) {
                        return GqlValue(-std::get<int64_t>(val.property));
                    }
                    if (std::holds_alternative<double>(val.property)) {
                        return GqlValue(-std::get<double>(val.property));
                    }
                }
                return GqlValue();
            }
            return GqlValue();
        }
        case ExpressionKind::BINARY_OP: {
            auto* bin = static_cast<const BinaryOpExpr*>(expr);
            if (bin->op == BinaryOpKind::AND) {
                auto lhs = evaluate_expression(row, bin->left.get());
                if (!lhs.is_truthy()) return GqlValue(false);
                auto rhs = evaluate_expression(row, bin->right.get());
                return GqlValue(rhs.is_truthy());
            }
            if (bin->op == BinaryOpKind::OR) {
                auto lhs = evaluate_expression(row, bin->left.get());
                if (lhs.is_truthy()) return GqlValue(true);
                auto rhs = evaluate_expression(row, bin->right.get());
                return GqlValue(rhs.is_truthy());
            }

            auto lhs = evaluate_expression(row, bin->left.get());
            auto rhs = evaluate_expression(row, bin->right.get());

            if (lhs.type == GqlValue::NIL || rhs.type == GqlValue::NIL) {
                return GqlValue();
            }

            if (bin->op == BinaryOpKind::EQ) {
                return GqlValue(compare_gql_values(lhs, rhs) == 0);
            }
            if (bin->op == BinaryOpKind::NE) {
                return GqlValue(compare_gql_values(lhs, rhs) != 0);
            }
            if (bin->op == BinaryOpKind::LT) {
                return GqlValue(compare_gql_values(lhs, rhs) < 0);
            }
            if (bin->op == BinaryOpKind::LE) {
                return GqlValue(compare_gql_values(lhs, rhs) <= 0);
            }
            if (bin->op == BinaryOpKind::GT) {
                return GqlValue(compare_gql_values(lhs, rhs) > 0);
            }
            if (bin->op == BinaryOpKind::GE) {
                return GqlValue(compare_gql_values(lhs, rhs) >= 0);
            }

            if (lhs.type == GqlValue::PROPERTY && rhs.type == GqlValue::PROPERTY) {
                if (std::holds_alternative<int64_t>(lhs.property) && std::holds_alternative<int64_t>(rhs.property)) {
                    int64_t l = std::get<int64_t>(lhs.property);
                    int64_t r = std::get<int64_t>(rhs.property);
                    if (bin->op == BinaryOpKind::ADD) return GqlValue(l + r);
                    if (bin->op == BinaryOpKind::SUB) return GqlValue(l - r);
                    if (bin->op == BinaryOpKind::MUL) return GqlValue(l * r);
                    if (bin->op == BinaryOpKind::DIV) return r != 0 ? GqlValue(l / r) : GqlValue();
                }
                if (std::holds_alternative<double>(lhs.property) || std::holds_alternative<double>(rhs.property)) {
                    double l = std::holds_alternative<double>(lhs.property) ? std::get<double>(lhs.property) : static_cast<double>(std::get<int64_t>(lhs.property));
                    double r = std::holds_alternative<double>(rhs.property) ? std::get<double>(rhs.property) : static_cast<double>(std::get<int64_t>(rhs.property));
                    if (bin->op == BinaryOpKind::ADD) return GqlValue(l + r);
                    if (bin->op == BinaryOpKind::SUB) return GqlValue(l - r);
                    if (bin->op == BinaryOpKind::MUL) return GqlValue(l * r);
                    if (bin->op == BinaryOpKind::DIV) return r != 0.0 ? GqlValue(l / r) : GqlValue();
                }
                if (std::holds_alternative<std::string>(lhs.property) && std::holds_alternative<std::string>(rhs.property)) {
                    if (bin->op == BinaryOpKind::ADD || bin->op == BinaryOpKind::CONCAT) {
                        return GqlValue(std::get<std::string>(lhs.property) + std::get<std::string>(rhs.property));
                    }
                }
                // String comparison operators
                if (std::holds_alternative<std::string>(lhs.property) && std::holds_alternative<std::string>(rhs.property)) {
                    const auto& l = std::get<std::string>(lhs.property);
                    const auto& r = std::get<std::string>(rhs.property);
                    if (bin->op == BinaryOpKind::STARTS_WITH) {
                        return GqlValue(l.rfind(r, 0) == 0);
                    }
                    if (bin->op == BinaryOpKind::ENDS_WITH) {
                        if (l.length() >= r.length()) {
                            return GqlValue(l.compare(l.length() - r.length(), r.length(), r) == 0);
                        }
                        return GqlValue(false);
                    }
                    if (bin->op == BinaryOpKind::CONTAINS) {
                        return GqlValue(l.find(r) != std::string::npos);
                    }
                }
            }
            return GqlValue();
        }
    }
    return GqlValue();
}

/**
 * @brief Serializes a GqlValue (Nil, Node, Relationship, or Property variant) to a JSON string.
 * 
 * Maps typed property types (booleans, integers, doubles, strings, array vectors) to
 * compliant JSON notations.
 * 
 * @param val The GqlValue to serialize.
 * @return std::string JSON representation.
 */
std::string serialize_gql_value(const GqlValue& val) {
    if (val.type == GqlValue::NIL) {
        return "null";
    }
    if (val.type == GqlValue::PROPERTY) {
        switch (val.property.index()) {
            case 0: return "null";
            case 1: return std::get<bool>(val.property) ? "true" : "false";
            case 2: return std::to_string(std::get<int64_t>(val.property));
            case 3: return std::to_string(std::get<double>(val.property));
            case 4: return seastar::json::formatter::to_json(std::get<std::string>(val.property));
            case 5: {
                std::string s = "[";
                bool init = true;
                for (bool x : std::get<std::vector<bool>>(val.property)) {
                    if (!init) s += ",";
                    s += (x ? "true" : "false");
                    init = false;
                }
                s += "]";
                return s;
            }
            case 6: {
                std::string s = "[";
                bool init = true;
                for (int64_t x : std::get<std::vector<int64_t>>(val.property)) {
                    if (!init) s += ",";
                    s += std::to_string(x);
                    init = false;
                }
                s += "]";
                return s;
            }
            case 7: {
                std::string s = "[";
                bool init = true;
                for (double x : std::get<std::vector<double>>(val.property)) {
                    if (!init) s += ",";
                    s += std::to_string(x);
                    init = false;
                }
                s += "]";
                return s;
            }
            case 8: {
                std::string s = "[";
                bool init = true;
                for (const auto& x : std::get<std::vector<std::string>>(val.property)) {
                    if (!init) s += ",";
                    s += seastar::json::formatter::to_json(x);
                    init = false;
                }
                s += "]";
                return s;
            }
        }
    }
    if (val.type == GqlValue::NODE) {
        std::string s = "{\"id\": " + std::to_string(val.node->getId()) + ", \"type\": \"" + val.node->getType() + "\", \"key\": \"" + val.node->getKey() + "\", \"properties\": {";
        bool init = true;
        for (const auto& [k, v] : val.node->getProperties()) {
            if (!init) s += ", ";
            s += "\"" + k + "\": " + serialize_gql_value(GqlValue(v));
            init = false;
        }
        s += "}}";
        return s;
    }
    if (val.type == GqlValue::RELATIONSHIP) {
        std::string s = "{\"id\": " + std::to_string(val.relationship->getId()) + ", \"type\": \"" + val.relationship->getType() + "\", \"from\": " + std::to_string(val.relationship->getStartingNodeId()) + ", \"to\": " + std::to_string(val.relationship->getEndingNodeId()) + ", \"properties\": {";
        bool init = true;
        for (const auto& [k, v] : val.relationship->getProperties()) {
            if (!init) s += ", ";
            s += "\"" + k + "\": " + serialize_gql_value(GqlValue(v));
            init = false;
        }
        s += "}}";
        return s;
    }
    if (val.type == GqlValue::RELATIONSHIP_LIST) {
        std::string s = "[";
        bool init = true;
        for (const auto& rel : *val.relationship_list) {
            if (!init) s += ",";
            s += serialize_gql_value(GqlValue(rel));
            init = false;
        }
        s += "]";
        return s;
    }
    // Serialize a heterogeneous list (collect_list result) to a JSON array.
    if (val.type == GqlValue::LIST) {
        std::string s = "[";
        bool init = true;
        for (const auto& item : *val.list) {
            if (!init) s += ", ";
            s += serialize_gql_value(item);
            init = false;
        }
        s += "]";
        return s;
    }
    // Serialize a Path to a JSON object containing lists of serialized nodes and relationships.
    if (val.type == GqlValue::PATH) {
        std::string s = "{\"nodes\": [";
        bool init = true;
        for (const auto& node : val.path->GetNodes()) {
            if (!init) s += ", ";
            s += serialize_gql_value(GqlValue(node));
            init = false;
        }
        s += "], \"relationships\": [";
        init = true;
        for (const auto& rel : val.path->GetRelationships()) {
            if (!init) s += ", ";
            s += serialize_gql_value(GqlValue(rel));
            init = false;
        }
        s += "]}";
        return s;
    }
    return "null";
}

/**
 * @brief Serializes a property map into a JSON object string.
 * 
 * @param props The property map to format.
 * @return std::string JSON object string.
 */
std::string serialize_properties_to_json(const std::map<std::string, property_type_t>& props) {
    std::string json = "{";
    bool first = true;
    for (const auto& [k, v] : props) {
        if (!first) json += ", ";
        json += "\"" + k + "\": " + serialize_gql_value(GqlValue(v));
        first = false;
    }
    json += "}";
    return json;
}

} // namespace ragedb::gql
