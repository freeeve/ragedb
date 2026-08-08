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

#ifndef RAGEDB_GQLASTEXPRESSIONS_H
#define RAGEDB_GQLASTEXPRESSIONS_H

// The expression half of the GQL AST: the ExpressionKind tag, the operator/aggregate/cast enums, and
// every Expression subclass. Split out of GqlAst.h, which keeps the pattern and query-level types and
// includes this first, so nothing outside the two files changes.
//
// ExistsExpr and SizeExpr are deliberately NOT here: both hold a MatchStatement, a pattern type, so they
// stay in GqlAst.h after that type is defined.

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <variant>
#include <optional>
#include <set>
#include "../graph/PropertyType.h"
#include "../graph/Operation.h"
#include "../graph/Direction.h"
#include "../graph/paths/Path.h"

namespace ragedb::gql {

enum class ExpressionKind {
    LITERAL,          ///< A static value (e.g. 5, "Alice", true)
    VARIABLE,         ///< An identifier reference (e.g. p, m)
    PROPERTY_LOOKUP,  ///< Property extraction (e.g. p.name, m.title)
    UNARY_OP,         ///< Unary operations (e.g. NOT, -x)
    BINARY_OP,        ///< Binary operations (e.g. AND, OR, +, =, <)
    AGGREGATION,      ///< GQL Aggregate function (e.g. COUNT, SUM, AVG, MIN, MAX)
    EXISTS,           ///< Exists subquery expression (e.g. EXISTS { MATCH ... })
    IS_NULL_CHECK,    ///< Null check expression (e.g. x IS NULL)
    SIZE_OP,          ///< Size function expression (e.g. size((x)-[:REL]->()))
    FUNCTION_CALL,    ///< Scalar function call (e.g. length(p), zoned_datetime('2010-01-01'))
    CASE_WHEN,        ///< CASE WHEN cond THEN val [WHEN ...] [ELSE val] END conditional expression
    IN_LIST,          ///< Membership test against a list value: x IN <listExpr>
    CAST,             ///< Type conversion: CAST(x AS STRING | INTEGER | FLOAT | BOOLEAN)
    IS_LABELED,       ///< Label predicate: x IS [NOT] LABELED <labelExpression>
    IS_DIRECTED,      ///< Edge orientation predicate: e IS [NOT] DIRECTED
    IS_SOURCE_DEST,   ///< Endpoint predicate: n IS [NOT] (SOURCE | DESTINATION) OF e
    LIST_LITERAL,     ///< A list value written out: [a, b, c]
    LIST_INDEX,       ///< List element access: list[index]
    LIST_COMPREHENSION, ///< [x IN list [WHERE pred] [| projection]]
    QUANTIFIED_PREDICATE, ///< all|any|none|single(x IN list WHERE pred)
    TEMPORAL_FIELD    ///< A temporal component accessor: <epoch-ms expr>.year/.month/.day/...
};

/**
 * @brief Target types for CAST(x AS T).
 */
enum class CastType {
    STRING,
    INTEGER,
    FLOAT,
    BOOLEAN
};

/**
 * @brief Types of aggregate functions supported by GQL.
 */
enum class AggregateKind {
    COUNT,
    SUM,
    AVG,
    MIN,
    MAX,
    COLLECT,     ///< collect_list: gather values into a LIST (DISTINCT dedups). GQL's general set function;
                 ///< the openCypher collect() spelling is rejected.
    STDDEV_POP,  ///< Population standard deviation: sqrt(sum((x-mean)^2) / n).
    STDDEV_SAMP, ///< Sample standard deviation: sqrt(sum((x-mean)^2) / (n-1)); NULL for n < 2.
    PERCENTILE_CONT, ///< Continuous percentile: interpolated value at fraction f. Binary: (value, f).
    PERCENTILE_DISC  ///< Discrete percentile: actual data value at fraction f. Binary: (value, f).
};

/**
 * @brief Unary operator kinds.
 */
enum class UnaryOpKind {
    NOT,  ///< Logical NOT (e.g. NOT condition)
    NEG   ///< Numeric Negation (e.g. -age)
};

/**
 * @brief Binary operator kinds.
 */
enum class BinaryOpKind {
    AND, OR,                 ///< Logical conjunction/disjunction
    ADD, SUB, MUL, DIV,      ///< Arithmetic operators (+, -, *, /)
    CONCAT,                  ///< String concatenation (||)
    EQ, NE, LT, LE, GT, GE,  ///< Comparison operators (=, !=, <, <=, >, >=)
    STARTS_WITH, ENDS_WITH,  ///< String comparisons
    CONTAINS,
    IS, AS                   ///< Keywords used in label specification and projections
};

/// Defined below with the pattern types; IS LABELED tests against one, so it is named here first.
struct LabelExpression;

/**
 * @brief Base struct for all GQL expression nodes.
 */
struct Expression {
    ExpressionKind kind;
    virtual ~Expression() = default;
    virtual std::unique_ptr<Expression> clone() const = 0;
};

/**
 * @brief Represents a literal value expression in the AST.
 */
struct LiteralExpr : public Expression {
    property_type_t value; ///< Holds the variant of property types (bool, string, int64_t, double, etc.)
    explicit LiteralExpr(property_type_t val) {
        kind = ExpressionKind::LITERAL;
        value = std::move(val);
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<LiteralExpr>(value);
    }
};

/**
 * @brief Represents a variable reference expression in the AST.
 */
struct VariableExpr : public Expression {
    std::string name; ///< The identifier of the referenced variable.
    explicit VariableExpr(std::string n) {
        kind = ExpressionKind::VARIABLE;
        name = std::move(n);
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<VariableExpr>(name);
    }
};

/**
 * @brief Represents a property retrieval from a variable (e.g., node.property).
 */
struct PropertyLookupExpr : public Expression {
    std::string variable; ///< Variable referencing the node/relationship.
    std::string property; ///< Property key to retrieve.
    PropertyLookupExpr(std::string var, std::string prop) {
        kind = ExpressionKind::PROPERTY_LOOKUP;
        variable = std::move(var);
        property = std::move(prop);
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<PropertyLookupExpr>(variable, property);
    }
};

/**
 * @brief Represents a unary operation expression.
 */
struct UnaryOpExpr : public Expression {
    UnaryOpKind op;                      ///< The unary operator kind.
    std::unique_ptr<Expression> expr;    ///< Target expression operand.
    UnaryOpExpr(UnaryOpKind o, std::unique_ptr<Expression> e) {
        kind = ExpressionKind::UNARY_OP;
        op = o;
        expr = std::move(e);
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<UnaryOpExpr>(op, expr ? expr->clone() : nullptr);
    }
};

/**
 * @brief Represents a binary operation expression.
 */
struct BinaryOpExpr : public Expression {
    BinaryOpKind op;                     ///< The binary operator kind.
    std::unique_ptr<Expression> left;    ///< Left expression operand.
    std::unique_ptr<Expression> right;   ///< Right expression operand.
    BinaryOpExpr(BinaryOpKind o, std::unique_ptr<Expression> l, std::unique_ptr<Expression> r) {
        kind = ExpressionKind::BINARY_OP;
        op = o;
        left = std::move(l);
        right = std::move(r);
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<BinaryOpExpr>(op, left ? left->clone() : nullptr, right ? right->clone() : nullptr);
    }
};

/**
 * @brief Represents a null check expression (e.g. value IS NULL, value IS NOT NULL).
 */
struct IsNullExpr : public Expression {
    std::unique_ptr<Expression> expr;
    bool is_not; // true for IS NOT NULL, false for IS NULL
    IsNullExpr(std::unique_ptr<Expression> e, bool not_val) {
        kind = ExpressionKind::IS_NULL_CHECK;
        expr = std::move(e);
        is_not = not_val;
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<IsNullExpr>(expr ? expr->clone() : nullptr, is_not);
    }
};

/**
 * @brief Represents an aggregate function expression (e.g. COUNT(p), SUM(p.age)).
 */
struct AggregateExpr : public Expression {
    AggregateKind fn_kind;              ///< The kind of aggregate function.
    std::unique_ptr<Expression> expr;   ///< Expression target to aggregate (nullptr for COUNT(*)).
    /// Second argument of a binary set function (PERCENTILE_CONT/DISC): the fraction in [0,1].
    std::unique_ptr<Expression> arg2;
    bool distinct = false;              ///< True for DISTINCT aggregates, e.g. count(DISTINCT x).
    /// True when a COUNT was rewritten into a degree SUM: the empty-input result must then stay
    /// count-shaped (0), not sum-shaped (null).
    bool count_to_sum = false;
    AggregateExpr(AggregateKind kind_val, std::unique_ptr<Expression> e, bool distinct_val = false) {
        kind = ExpressionKind::AGGREGATION;
        fn_kind = kind_val;
        expr = std::move(e);
        distinct = distinct_val;
    }
    std::unique_ptr<Expression> clone() const override {
        auto copy = std::make_unique<AggregateExpr>(fn_kind, expr ? expr->clone() : nullptr, distinct);
        copy->count_to_sum = count_to_sum;
        copy->arg2 = arg2 ? arg2->clone() : nullptr;
        return copy;
    }
};

/**
 * @brief Represents a scalar function call (e.g. length(p), zoned_datetime('2010-01-01')).
 *        The function name is stored lowercased for case-insensitive dispatch.
 */
struct FunctionCallExpr : public Expression {
    std::string name;                                    ///< Lowercased function name.
    std::vector<std::unique_ptr<Expression>> args;       ///< Argument expressions.
    FunctionCallExpr(std::string n, std::vector<std::unique_ptr<Expression>> a) {
        kind = ExpressionKind::FUNCTION_CALL;
        name = std::move(n);
        args = std::move(a);
    }
    std::unique_ptr<Expression> clone() const override {
        std::vector<std::unique_ptr<Expression>> copy_args;
        copy_args.reserve(args.size());
        for (const auto& a : args) copy_args.push_back(a ? a->clone() : nullptr);
        return std::make_unique<FunctionCallExpr>(name, std::move(copy_args));
    }
};

/**
 * @brief Membership test against a list value: `x IN <listExpr>` (e.g. `t IN before` where `before`
 *        is a collect_list result). The literal form `x IN [a, b]` is desugared to an OR chain in the
 *        parser instead; this node is for a list-valued right operand.
 */
struct InExpr : public Expression {
    std::unique_ptr<Expression> value;   ///< The candidate element.
    std::unique_ptr<Expression> list;    ///< The list-valued expression to search.
    InExpr(std::unique_ptr<Expression> v, std::unique_ptr<Expression> l) {
        kind = ExpressionKind::IN_LIST;
        value = std::move(v);
        list = std::move(l);
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<InExpr>(value ? value->clone() : nullptr, list ? list->clone() : nullptr);
    }
};

/**
 * @brief Represents a list literal: [a, b, c]. Evaluates to a LIST value, so it can be bound, compared, or
 *        expanded by FOR.
 */
struct ListExpr : public Expression {
    std::vector<std::unique_ptr<Expression>> elements;
    explicit ListExpr(std::vector<std::unique_ptr<Expression>> e) {
        kind = ExpressionKind::LIST_LITERAL;
        elements = std::move(e);
    }
    std::unique_ptr<Expression> clone() const override {
        std::vector<std::unique_ptr<Expression>> copy;
        copy.reserve(elements.size());
        for (const auto& e : elements) {
            copy.push_back(e ? e->clone() : nullptr);
        }
        return std::make_unique<ListExpr>(std::move(copy));
    }
};

/**
 * @brief List element access: list[index]. 0-based; a negative index counts from the end; an
 * out-of-range index (or a non-list / non-integer operand) evaluates to NULL.
 */
struct IndexExpr : public Expression {
    std::unique_ptr<Expression> list;  ///< The list-valued operand.
    std::unique_ptr<Expression> index; ///< The integer index expression.
    IndexExpr(std::unique_ptr<Expression> l, std::unique_ptr<Expression> i) {
        kind = ExpressionKind::LIST_INDEX;
        list = std::move(l);
        index = std::move(i);
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<IndexExpr>(list ? list->clone() : nullptr,
                                           index ? index->clone() : nullptr);
    }
};

/**
 * @brief List comprehension: [variable IN list [WHERE filter] [| projection]]. For each element of `list`,
 * `variable` is bound to it in a scoped row; elements failing `filter` are dropped; the result is the list
 * of `projection` values (or the surviving elements themselves when `projection` is null).
 */
struct ListComprehensionExpr : public Expression {
    std::string variable;                  ///< The iteration variable, bound per element.
    std::unique_ptr<Expression> list;      ///< The source list expression.
    std::unique_ptr<Expression> filter;    ///< Optional WHERE predicate (nullable).
    std::unique_ptr<Expression> projection;///< Optional `| expr` value (nullable -> the element itself).
    ListComprehensionExpr(std::string v, std::unique_ptr<Expression> l,
                          std::unique_ptr<Expression> f, std::unique_ptr<Expression> p) {
        kind = ExpressionKind::LIST_COMPREHENSION;
        variable = std::move(v);
        list = std::move(l);
        filter = std::move(f);
        projection = std::move(p);
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<ListComprehensionExpr>(variable, list ? list->clone() : nullptr,
                                                       filter ? filter->clone() : nullptr,
                                                       projection ? projection->clone() : nullptr);
    }
};

/**
 * @brief Quantified list predicate: all|any|none|single(variable IN list WHERE predicate). Binds
 * `variable` to each element in a scoped row and tests `predicate`; the quantifier reduces to a boolean.
 */
struct QuantifiedPredicateExpr : public Expression {
    enum Quantifier { ALL, ANY, NONE, SINGLE };
    Quantifier quant;
    std::string variable;
    std::unique_ptr<Expression> list;
    std::unique_ptr<Expression> predicate;
    QuantifiedPredicateExpr(Quantifier q, std::string v, std::unique_ptr<Expression> l,
                            std::unique_ptr<Expression> p) {
        kind = ExpressionKind::QUANTIFIED_PREDICATE;
        quant = q;
        variable = std::move(v);
        list = std::move(l);
        predicate = std::move(p);
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<QuantifiedPredicateExpr>(quant, variable, list ? list->clone() : nullptr,
                                                         predicate ? predicate->clone() : nullptr);
    }
};

/**
 * @brief Temporal component accessor: <value>.year/.month/.day/.hour/.minute/.second, where <value> is an
 * epoch-millisecond datetime. Extracts the named UTC calendar/clock field as an integer.
 */
struct TemporalFieldExpr : public Expression {
    std::unique_ptr<Expression> value; ///< The epoch-ms datetime expression.
    std::string field;                 ///< Lowercased component name (year/month/day/hour/minute/second).
    TemporalFieldExpr(std::unique_ptr<Expression> v, std::string f) {
        kind = ExpressionKind::TEMPORAL_FIELD;
        value = std::move(v);
        field = std::move(f);
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<TemporalFieldExpr>(value ? value->clone() : nullptr, field);
    }
};

/**
 * @brief Represents CAST(<expr> AS <type>): converts a value to the named type, yielding NULL when the
 *        value cannot be represented in it (e.g. CAST('abc' AS INTEGER)).
 */
struct CastExpr : public Expression {
    std::unique_ptr<Expression> value;   ///< The value being converted.
    CastType target;                     ///< The type to convert to.
    CastExpr(std::unique_ptr<Expression> v, CastType t) {
        kind = ExpressionKind::CAST;
        value = std::move(v);
        target = t;
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<CastExpr>(value ? value->clone() : nullptr, target);
    }
};

/**
 * @brief Represents `<expr> IS [NOT] LABELED <labelExpression>`: whether a node or relationship carries
 *        the given label. The label side reuses the pattern label expression, so AND/OR/NOT/`%` compose
 *        exactly as they do inside a pattern.
 */
struct IsLabeledExpr : public Expression {
    std::unique_ptr<Expression> value;               ///< The node or relationship being tested.
    std::shared_ptr<LabelExpression> label_expr;     ///< The label expression to test against.
    bool negated = false;                            ///< True for IS NOT LABELED.
    IsLabeledExpr(std::unique_ptr<Expression> v, std::shared_ptr<LabelExpression> l, bool n) {
        kind = ExpressionKind::IS_LABELED;
        value = std::move(v);
        label_expr = std::move(l);
        negated = n;
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<IsLabeledExpr>(value ? value->clone() : nullptr, label_expr, negated);
    }
};

/**
 * @brief `<expr> IS [NOT] DIRECTED`: whether a relationship is directed. ragedb relationships are
 *        always directed, so this is true (false when negated) for a relationship, null otherwise.
 */
struct IsDirectedExpr : public Expression {
    std::unique_ptr<Expression> value;  ///< The relationship being tested.
    bool negated = false;               ///< True for IS NOT DIRECTED.
    IsDirectedExpr(std::unique_ptr<Expression> v, bool n) {
        kind = ExpressionKind::IS_DIRECTED;
        value = std::move(v);
        negated = n;
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<IsDirectedExpr>(value ? value->clone() : nullptr, negated);
    }
};

/**
 * @brief `<node> IS [NOT] (SOURCE | DESTINATION) OF <edge>`: whether the node is the start (source)
 *        or end (destination) node of the relationship.
 */
struct IsSourceDestExpr : public Expression {
    std::unique_ptr<Expression> value;  ///< The node being tested.
    std::unique_ptr<Expression> edge;   ///< The relationship whose endpoint is checked.
    bool is_source = true;              ///< True for SOURCE OF, false for DESTINATION OF.
    bool negated = false;               ///< True for IS NOT ... OF.
    IsSourceDestExpr(std::unique_ptr<Expression> v, std::unique_ptr<Expression> e, bool src, bool n) {
        kind = ExpressionKind::IS_SOURCE_DEST;
        value = std::move(v);
        edge = std::move(e);
        is_source = src;
        negated = n;
    }
    std::unique_ptr<Expression> clone() const override {
        return std::make_unique<IsSourceDestExpr>(value ? value->clone() : nullptr,
                                                  edge ? edge->clone() : nullptr, is_source, negated);
    }
};

/**
 * @brief Represents a searched CASE expression: CASE WHEN c1 THEN v1 [WHEN c2 THEN v2 ...] [ELSE e] END.
 */
struct CaseExpr : public Expression {
    std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> branches; ///< (when, then) pairs.
    std::unique_ptr<Expression> else_expr;               ///< Optional ELSE result (nullptr => NULL).
    CaseExpr(std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> b,
             std::unique_ptr<Expression> e) {
        kind = ExpressionKind::CASE_WHEN;
        branches = std::move(b);
        else_expr = std::move(e);
    }
    std::unique_ptr<Expression> clone() const override {
        std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> copy_branches;
        copy_branches.reserve(branches.size());
        for (const auto& [w, t] : branches) {
            copy_branches.emplace_back(w ? w->clone() : nullptr, t ? t->clone() : nullptr);
        }
        return std::make_unique<CaseExpr>(std::move(copy_branches), else_expr ? else_expr->clone() : nullptr);
    }
};

}  // namespace ragedb::gql

#endif  // RAGEDB_GQLASTEXPRESSIONS_H
