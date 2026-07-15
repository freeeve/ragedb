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

#include "GqlParser.h"
#include "GqlValue.h"
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <optional>

namespace ragedb::gql {

/**
 * @brief Peek at a token in the stream without advancing the cursor.
 * 
 * @param offset The number of tokens ahead of the current position to inspect.
 * @return const Token& The token at the specified offset, or the EOF/last token if out of bounds.
 */
const Token& GqlParser::peek(size_t offset) const {
    if (pos + offset >= tokens.size()) {
        return tokens.back();
    }
    return tokens[pos + offset];
}

/**
 * @brief Consume the current token and advance the stream position.
 * 
 * @return const Token& The token that was current before advancing.
 */
const Token& GqlParser::advance() {
    if (pos < tokens.size()) {
        pos++;
    }
    return tokens[pos - 1];
}

/**
 * @brief Check if the current token matches the specified type.
 * 
 * @param type The token type to compare against.
 * @return true If the current token's type matches.
 * @return false Otherwise.
 */
bool GqlParser::check(TokenType type) const {
    return peek().type == type;
}

/**
 * @brief If the current token matches the specified type, consume it and return true.
 * 
 * @param type The token type to match.
 * @return true If the token was matched and consumed.
 * @return false If the token did not match, leaving the cursor unchanged.
 */
bool GqlParser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

/**
 * @brief Assert that the current token matches the expected type and consume it.
 *        Throws an exception if the token type does not match.
 * 
 * @param type The expected token type.
 * @param error_message The descriptive message to include in the thrown exception.
 * @throws std::runtime_error If the current token type does not match.
 */
void GqlParser::consume(TokenType type, const std::string& error_message) {
    if (check(type)) {
        advance();
        return;
    }
    throw std::runtime_error(error_message + " (found: " + peek().text + ")");
}

/**
 * @brief Consume the current token as an identifier and return its text. Accepts NAME plus any
 *        keyword token, so reserved words (e.g. `with`) remain usable as aliases and names.
 *
 * @param error_message The descriptive message to include in the thrown exception.
 * @throws std::runtime_error If the current token is not identifier-like.
 */
std::string GqlParser::consume_identifier(const std::string& error_message) {
    TokenType type = peek().type;
    if (type == TokenType::NAME || (type >= TokenType::TRUE_KW && type < TokenType::LPAREN)) {
        std::string text = peek().text;
        advance();
        return text;
    }
    throw std::runtime_error(error_message + " (found: " + peek().text + ")");
}

/**
 * @brief Parse a comma-separated projection list (RETURN or WITH items) into query.returns.
 *        WITH items pass require_alias_for_expressions: a projected column's name becomes the
 *        binding the next segment resolves, so anything other than a plain variable has no usable
 *        name of its own and must be aliased.
 */
void GqlParser::parse_return_items(GqlQuery& query, bool require_alias_for_expressions) {
    do {
        ReturnItem item;
        item.expr = parse_expression();
        if (match(TokenType::AS)) {
            item.alias = consume_identifier("Expected alias name after 'AS'");
        }
        if (require_alias_for_expressions && !item.alias && item.expr->kind != ExpressionKind::VARIABLE) {
            throw std::runtime_error("WITH items other than a plain variable must be aliased with AS");
        }
        query.returns.push_back(std::move(item));
    } while (match(TokenType::COMMA));
}

/**
 * @brief Replace references to the projection's output aliases inside an ORDER BY sort key with
 *        clones of the aliased expressions. ORDER BY is evaluated after the projection, so its
 *        aliases are in scope for the sort keys (as in Cypher and SQL); the executor evaluates
 *        sort keys against pre-projection rows, so the substitution reconstructs that scope.
 *        Aggregation arguments are not descended into (an alias cannot appear there).
 */
static std::unique_ptr<Expression> substitute_return_aliases(
        std::unique_ptr<Expression> expr, const std::map<std::string, const Expression*>& aliases) {
    if (!expr) return expr;
    switch (expr->kind) {
        case ExpressionKind::VARIABLE: {
            auto it = aliases.find(static_cast<const VariableExpr*>(expr.get())->name);
            if (it != aliases.end()) {
                return it->second->clone();
            }
            return expr;
        }
        case ExpressionKind::UNARY_OP: {
            auto* un = static_cast<UnaryOpExpr*>(expr.get());
            un->expr = substitute_return_aliases(std::move(un->expr), aliases);
            return expr;
        }
        case ExpressionKind::BINARY_OP: {
            auto* bin = static_cast<BinaryOpExpr*>(expr.get());
            bin->left = substitute_return_aliases(std::move(bin->left), aliases);
            bin->right = substitute_return_aliases(std::move(bin->right), aliases);
            return expr;
        }
        case ExpressionKind::FUNCTION_CALL: {
            auto* fc = static_cast<FunctionCallExpr*>(expr.get());
            for (auto& a : fc->args) {
                a = substitute_return_aliases(std::move(a), aliases);
            }
            return expr;
        }
        case ExpressionKind::CASE_WHEN: {
            auto* ce = static_cast<CaseExpr*>(expr.get());
            for (auto& b : ce->branches) {
                b.first = substitute_return_aliases(std::move(b.first), aliases);
                b.second = substitute_return_aliases(std::move(b.second), aliases);
            }
            if (ce->else_expr) {
                ce->else_expr = substitute_return_aliases(std::move(ce->else_expr), aliases);
            }
            return expr;
        }
        case ExpressionKind::IN_LIST: {
            auto* in = static_cast<InExpr*>(expr.get());
            in->value = substitute_return_aliases(std::move(in->value), aliases);
            in->list = substitute_return_aliases(std::move(in->list), aliases);
            return expr;
        }
        case ExpressionKind::CAST: {
            auto* c = static_cast<CastExpr*>(expr.get());
            c->value = substitute_return_aliases(std::move(c->value), aliases);
            return expr;
        }
        case ExpressionKind::IS_LABELED: {
            auto* l = static_cast<IsLabeledExpr*>(expr.get());
            l->value = substitute_return_aliases(std::move(l->value), aliases);
            return expr;
        }
        case ExpressionKind::LIST_LITERAL: {
            auto* le = static_cast<ListExpr*>(expr.get());
            for (auto& element : le->elements) {
                element = substitute_return_aliases(std::move(element), aliases);
            }
            return expr;
        }
        default:
            return expr;
    }
}

/**
 * @brief Bind the RETURN/WITH projection's output aliases into the ORDER BY sort-key scope by
 *        substituting them with their defining expressions.
 */
static void resolve_order_by_aliases(GqlQuery& query) {
    if (query.order_by.empty() || query.returns.empty()) return;
    std::map<std::string, const Expression*> aliases;
    for (const auto& item : query.returns) {
        if (item.alias && item.expr) {
            aliases[*item.alias] = item.expr.get();
        }
    }
    if (aliases.empty()) return;
    for (auto& spec : query.order_by) {
        spec.expr = substitute_return_aliases(std::move(spec.expr), aliases);
    }
}

/**
 * @brief Parse an optional ORDER BY clause (with per-key ASC/DESC) into query.order_by.
 */
void GqlParser::parse_order_by(GqlQuery& query) {
    if (!match(TokenType::ORDER_BY)) return;
    do {
        SortSpec spec;
        spec.expr = parse_expression();
        if (match(TokenType::ASC)) {
            spec.ascending = true;
        } else if (match(TokenType::DESC)) {
            spec.ascending = false;
        } else {
            spec.ascending = true;
        }
        query.order_by.push_back(std::move(spec));
    } while (match(TokenType::COMMA));
}

/**
 * @brief Parse the optional OFFSET and LIMIT page clauses into query.offset / query.limit. Either order is
 *        accepted, so both `OFFSET 10 LIMIT 5` and `LIMIT 5 OFFSET 10` parse.
 */
void GqlParser::parse_limit(GqlQuery& query) {
    for (;;) {
        if (match(TokenType::LIMIT)) {
            std::string num_str = peek().text;
            consume(TokenType::NUMBER, "Expected integer limit value");
            query.limit = std::stoull(num_str);
        } else if (match(TokenType::OFFSET)) {
            std::string num_str = peek().text;
            consume(TokenType::NUMBER, "Expected integer offset value");
            query.offset = std::stoull(num_str);
        } else {
            return;
        }
    }
}

static void validate_insert_label_expr(const std::shared_ptr<LabelExpression>& expr) {
    if (!expr) return;
    if (expr->kind != LabelExprKind::LITERAL) {
        throw std::runtime_error("Complex label expressions (AND, OR, NOT) are not allowed in INSERT statements");
    }
}

/**
 * @brief Parses a complete GQL query into a GqlQuery AST object.
 * 
 * Main entry point of the recursive descent parser. Processes MATCH, OPTIONAL MATCH,
 * global WHERE, write operations (INSERT, SET, REMOVE, DELETE, DETACH DELETE),
 * and RETURN clauses with sorting and limits.
 * 
 * @return GqlQuery The constructed query AST object.
 * @throws std::runtime_error If syntax errors are encountered.
 */
GqlQuery GqlParser::parse_single_query() {
    std::vector<std::shared_ptr<GqlQuery>> with_segments;
    std::unique_ptr<Expression> pending_having;
    int match_id_counter = 0;
    int optional_group_counter = 0;

    while (true) {
    GqlQuery query;
    // A WHERE that followed a preceding WITH filters the rows piped into this segment.
    if (pending_having) {
        query.where_expr = std::move(pending_having);
    }
    // Parse MATCH/OPTIONAL MATCH clauses. A WHERE may sit between MATCH groups
    // (MATCH ... WHERE ... MATCH ... RETURN): after each group we take an optional WHERE and loop
    // back so a following MATCH keeps extending the same segment.
    while (true) {
    while (check(TokenType::MATCH) || (check(TokenType::OPTIONAL) && peek(1).type == TokenType::MATCH)) {
        MatchStatement stmt;
        if (match(TokenType::OPTIONAL)) {
            stmt.is_optional = true;
        }
        consume(TokenType::MATCH, "Expected MATCH");

        // Parse GQL Match Mode
        if (match(TokenType::DIFFERENT_KW)) {
            if (check(TokenType::NAME) && (peek().text == "EDGES" || peek().text == "edges")) {
                advance();
            } else {
                throw std::runtime_error("Expected 'EDGES' after 'DIFFERENT'");
            }
            stmt.match_mode = MatchMode::DIFFERENT_EDGES;
        } else if (match(TokenType::REPEATABLE_KW)) {
            if (check(TokenType::NAME) && (peek().text == "ELEMENTS" || peek().text == "elements")) {
                advance();
            } else {
                throw std::runtime_error("Expected 'ELEMENTS' after 'REPEATABLE'");
            }
            stmt.match_mode = MatchMode::REPEATABLE_ELEMENTS;
        }

        // Parse GQL Path Mode
        if (match(TokenType::TRAIL_KW)) {
            stmt.path_mode = PathMode::TRAIL;
        } else if (match(TokenType::ACYCLIC_KW)) {
            stmt.path_mode = PathMode::ACYCLIC;
        } else if (match(TokenType::SIMPLE_KW)) {
            stmt.path_mode = PathMode::SIMPLE;
        } else if (match(TokenType::WALK_KW)) {
            stmt.path_mode = PathMode::WALK;
        }

        if (match(TokenType::KHOP)) {
            stmt.is_khop = true;
        }
        // Parse optional path variable assignment (e.g. p = ALL SHORTEST ...)
        if (check(TokenType::NAME) && peek(1).type == TokenType::EQ) {
            stmt.path_variable = peek().text;
            advance(); // Consume variable
            advance(); // Consume '='

            // Parse shortest path selectors:
            // 1. ALL SHORTEST paths or ALL CHEAPEST paths
            if (check(TokenType::ALL_KW) && peek(1).type == TokenType::SHORTEST_KW) {
                stmt.shortest_path_kind = ShortestPathKind::ALL;
                advance();
                advance();
            } else if (check(TokenType::ALL_KW) && peek(1).type == TokenType::CHEAPEST_KW) {
                stmt.shortest_path_kind = ShortestPathKind::ALL_CHEAPEST;
                advance();
                advance();
                if (check(TokenType::NAME) && (peek().text == "PATH" || peek().text == "path" || peek().text == "PATHS" || peek().text == "paths")) {
                    advance();
                }
            // 2. ANY SHORTEST paths or ANY CHEAPEST paths
            } else if (check(TokenType::ANY_KW) && peek(1).type == TokenType::SHORTEST_KW) {
                stmt.shortest_path_kind = ShortestPathKind::ANY;
                advance();
                advance();
            } else if (check(TokenType::ANY_KW) && peek(1).type == TokenType::CHEAPEST_KW) {
                stmt.shortest_path_kind = ShortestPathKind::CHEAPEST;
                advance();
                advance();
                if (check(TokenType::NAME) && (peek().text == "PATH" || peek().text == "path")) {
                    advance();
                }
            // 3. SHORTEST k or SHORTEST k GROUP
            } else if (check(TokenType::SHORTEST_KW)) {
                advance(); // consume SHORTEST
                uint64_t k = 1;
                if (check(TokenType::NUMBER)) {
                    k = peek().int_value;
                    advance();
                }
                stmt.shortest_path_k = k;
                if (match(TokenType::GROUP_KW)) {
                    stmt.shortest_path_kind = ShortestPathKind::K_GROUP;
                } else {
                    stmt.shortest_path_kind = ShortestPathKind::K;
                }
            // 4. CHEAPEST [PATH] or CHEAPEST k [PATH[S]]
            } else if (match(TokenType::CHEAPEST_KW)) {
                uint64_t k = 1;
                if (check(TokenType::NUMBER)) {
                    k = peek().int_value;
                    advance();
                    stmt.shortest_path_kind = ShortestPathKind::CHEAPEST_K;
                    stmt.shortest_path_k = k;
                    if (check(TokenType::NAME) && (peek().text == "PATH" || peek().text == "path" || peek().text == "PATHS" || peek().text == "paths")) {
                        advance();
                    }
                } else {
                    stmt.shortest_path_kind = ShortestPathKind::CHEAPEST;
                    if (check(TokenType::NAME) && (peek().text == "PATH" || peek().text == "path")) {
                        advance();
                    }
                }
            }
        }
        if (check(TokenType::NAME) && peek(1).type == TokenType::IN_KW) {
            stmt.is_search = true;
            stmt.search_var = peek().text;
            consume(TokenType::NAME, "Expected variable name before 'IN'");
            consume(TokenType::IN_KW, "Expected 'IN'");
            consume(TokenType::SEARCH, "Expected 'SEARCH'");
            
            if (match(TokenType::LPAREN)) {
                do {
                    std::string type_name = peek().text;
                    consume(TokenType::NAME, "Expected type name");
                    consume(TokenType::DOT, "Expected '.'");
                    std::string prop_name = peek().text;
                    consume(TokenType::NAME, "Expected property name");
                    
                    stmt.search_type = type_name;
                    stmt.search_properties.push_back(prop_name);
                } while (match(TokenType::COMMA));
                consume(TokenType::RPAREN, "Expected ')' after property list");
            } else {
                std::string type_name = peek().text;
                consume(TokenType::NAME, "Expected type name");
                consume(TokenType::DOT, "Expected '.'");
                std::string prop_name = peek().text;
                consume(TokenType::NAME, "Expected property name");
                
                stmt.search_type = type_name;
                stmt.search_properties.push_back(prop_name);
            }
            
            consume(TokenType::FOR, "Expected 'FOR'");
            stmt.search_string = peek().text;
            consume(TokenType::STRING_LIT, "Expected query string literal after 'FOR'");
            
            if (match(TokenType::OPTIONS)) {
                consume(TokenType::LBRACE, "Expected '{' after OPTIONS");
                if (!check(TokenType::RBRACE)) {
                    do {
                        std::string opt_key = peek().text;
                        consume(TokenType::NAME, "Expected option key identifier");
                        consume(TokenType::COLON, "Expected ':' after option key");
                        
                        std::string opt_val;
                        if (check(TokenType::STRING_LIT)) {
                            opt_val = peek().text;
                            consume(TokenType::STRING_LIT, "Expected string literal");
                        } else if (check(TokenType::TRUE_KW)) {
                            opt_val = "true";
                            consume(TokenType::TRUE_KW, "Expected true");
                        } else if (check(TokenType::FALSE_KW)) {
                            opt_val = "false";
                            consume(TokenType::FALSE_KW, "Expected false");
                        } else if (check(TokenType::NAME)) {
                            opt_val = peek().text;
                            consume(TokenType::NAME, "Expected name identifier");
                        } else {
                            throw std::runtime_error("Expected option value");
                        }
                        stmt.search_options[opt_key] = opt_val;
                    } while (match(TokenType::COMMA));
                }
                consume(TokenType::RBRACE, "Expected '}' to close OPTIONS");
            }
            
            consume(TokenType::YIELD, "Expected 'YIELD'");
            stmt.yield_var = peek().text;
            consume(TokenType::NAME, "Expected variable name to bind search result");
            consume(TokenType::COMMA, "Expected ','");
            stmt.yield_score_var = peek().text;
            consume(TokenType::NAME, "Expected score variable name");
            stmt.id = match_id_counter++;
            query.matches.push_back(std::move(stmt));
        } else {
            int group_id = stmt.is_optional ? optional_group_counter++ : -1;
            do {
                MatchStatement current_stmt = stmt;
                current_stmt.pattern = parse_path_pattern();
                if (current_stmt.pattern.is_questioned) {
                    current_stmt.is_optional = true;
                    current_stmt.optional_group_id = (group_id >= 0) ? group_id : optional_group_counter++;
                } else {
                    current_stmt.optional_group_id = group_id;
                }
                if (current_stmt.shortest_path_kind == ShortestPathKind::CHEAPEST ||
                    current_stmt.shortest_path_kind == ShortestPathKind::ALL_CHEAPEST ||
                    current_stmt.shortest_path_kind == ShortestPathKind::CHEAPEST_K) {
                    // Backwards compatibility for old-style COST clause placed outside the path pattern
                    if (match(TokenType::COST_KW)) {
                        auto expr = parse_expression();
                        if (!current_stmt.pattern.edges.empty()) {
                            current_stmt.pattern.edges[0].cost_expr = std::move(expr);
                        }
                    }
                }
                current_stmt.id = match_id_counter++;
                query.matches.push_back(std::move(current_stmt));
            } while (match(TokenType::COMMA));
        }
    }

    // A WHERE after a MATCH group filters those rows; the ISO GQL FILTER statement is its synonym on
    // the working table. AND-combine either with any HAVING carried in from a prior WITH/NEXT and with
    // earlier interleaved predicates in this segment.
    if (match(TokenType::WHERE) || match(TokenType::FILTER_KW)) {
        auto w = parse_expression();
        if (query.where_expr) {
            query.where_expr = std::make_unique<BinaryOpExpr>(BinaryOpKind::AND, std::move(query.where_expr), std::move(w));
        } else {
            query.where_expr = std::move(w);
        }
    } else if (match(TokenType::LET_KW)) {
        // ISO GQL LET adds computed bindings to the working table, usable by the rest of the segment.
        do {
            ReturnItem item;
            item.alias = consume_identifier("Expected variable name after 'LET'");
            consume(TokenType::EQ, "Expected '=' after LET variable");
            item.expr = parse_expression();
            query.let_bindings.push_back(std::move(item));
        } while (match(TokenType::COMMA));
    } else if (match(TokenType::FOR)) {
        // ISO GQL FOR expands a list into one row per element (the standard's UNWIND). Unlike LET, which
        // adds a column, FOR multiplies the working table's rows.
        do {
            ForBinding binding;
            binding.variable = consume_identifier("Expected variable name after 'FOR'");
            consume(TokenType::IN_KW, "Expected 'IN' after FOR variable");
            binding.list_expr = parse_expression();
            query.for_bindings.push_back(std::move(binding));
        } while (match(TokenType::COMMA));
    }
    // A segment is a free sequence of simple statements (MATCH / FILTER / LET / FOR); keep extending it
    // while more of them follow. MATCH ... WHERE ... MATCH ... and FILTER ... MATCH ... both round-trip here.
    if (check(TokenType::MATCH) || (check(TokenType::OPTIONAL) && peek(1).type == TokenType::MATCH) ||
        check(TokenType::WHERE) || check(TokenType::FILTER_KW) || check(TokenType::LET_KW) ||
        check(TokenType::FOR)) {
        continue;
    }
    break;
    }

    // Parse Write Operations (INSERT, SET, REMOVE, DELETE, DETACH)
    while (check(TokenType::INSERT) || check(TokenType::SET) || check(TokenType::REMOVE) || check(TokenType::DELETE) || check(TokenType::DETACH)) {
        if (match(TokenType::INSERT)) {
            WriteOp op;
            op.type = WriteOp::Type::INSERT;
            op.insert_pattern = parse_path_pattern();
            for (const auto& node : op.insert_pattern.nodes) {
                validate_insert_label_expr(node.label_expr);
            }
            for (const auto& edge : op.insert_pattern.edges) {
                validate_insert_label_expr(edge.label_expr);
            }
            query.writes.push_back(std::move(op));
        } else if (match(TokenType::SET)) {
            do {
                std::string var = peek().text;
                consume(TokenType::NAME, "Expected variable name in SET");
                consume(TokenType::DOT, "Expected '.' after variable in SET");
                std::string prop = peek().text;
                consume(TokenType::NAME, "Expected property name in SET");
                consume(TokenType::EQ, "Expected '=' after property in SET");
                auto expr = parse_expression();

                WriteOp op;
                op.type = WriteOp::Type::SET;
                op.set_var = var;
                op.set_prop = prop;
                op.set_expr = std::move(expr);
                query.writes.push_back(std::move(op));
            } while (match(TokenType::COMMA));
        } else if (match(TokenType::REMOVE)) {
            do {
                std::string var = peek().text;
                consume(TokenType::NAME, "Expected variable name in REMOVE");
                consume(TokenType::DOT, "Expected '.' after variable in REMOVE");
                std::string prop = peek().text;
                consume(TokenType::NAME, "Expected property name in REMOVE");

                WriteOp op;
                op.type = WriteOp::Type::REMOVE;
                op.remove_var = var;
                op.remove_prop = prop;
                query.writes.push_back(std::move(op));
            } while (match(TokenType::COMMA));
        } else {
            bool detach = false;
            if (match(TokenType::DETACH)) {
                detach = true;
                consume(TokenType::DELETE, "Expected 'DELETE' after 'DETACH'");
            } else {
                consume(TokenType::DELETE, "Expected 'DELETE'");
            }
            do {
                std::string var = peek().text;
                consume(TokenType::NAME, "Expected variable name to delete");

                WriteOp op;
                op.type = WriteOp::Type::DELETE_OP;
                op.delete_var = var;
                op.detach = detach;
                query.writes.push_back(std::move(op));
            } while (match(TokenType::COMMA));
        }
    }

    // Verify we have at least one MATCH or write operation (a continuation segment after WITH may
    // have neither--it operates on the rows piped in from the previous segment). A FOR statement is
    // also a row source in its own right: `FOR x IN [1, 2, 3] RETURN x` matches nothing but still
    // produces rows.
    if (query.matches.empty() && query.writes.empty() && with_segments.empty() && query.for_bindings.empty()) {
        throw std::runtime_error("Query must contain at least one MATCH or write clause");
    }

    // ISO GQL standalone ordering/paging statement: a segment may begin with ORDER BY [LIMIT] operating
    // on the working table piped in from the previous segment (no MATCH/RETURN of its own yet).
    if ((check(TokenType::ORDER_BY) || check(TokenType::LIMIT) || check(TokenType::OFFSET)) &&
        query.matches.empty() && query.returns.empty() && query.let_bindings.empty() &&
        !with_segments.empty()) {
        parse_order_by(query);
        parse_limit(query);
        // If a MATCH follows, the sort/page is a segment boundary: it must forward the sorted+limited
        // working table to the next MATCH. Synthesize a passthrough projection of the previous segment's
        // output columns so the existing projection pipeline carries every binding forward.
        if (check(TokenType::MATCH) || (check(TokenType::OPTIONAL) && peek(1).type == TokenType::MATCH)) {
            for (const auto& item : with_segments.back()->returns) {
                std::string col = item.alias
                    ? *item.alias
                    : (item.expr && item.expr->kind == ExpressionKind::VARIABLE
                           ? static_cast<VariableExpr*>(item.expr.get())->name
                           : std::string());
                if (!col.empty()) {
                    ReturnItem pass;
                    pass.expr = std::make_unique<VariableExpr>(col);
                    pass.alias = col;
                    query.returns.push_back(std::move(pass));
                }
            }
            resolve_order_by_aliases(query);
            with_segments.push_back(std::make_shared<GqlQuery>(std::move(query)));
            continue;
        }
        // Otherwise a RETURN follows and re-projects the ordered/paged rows. Push the ORDER BY/LIMIT
        // into the producing (previous) segment so its streaming top-K bounds the sort during the
        // traversal, instead of materialising the whole intermediate to sort it here. Valid
        // because the order keys are the previous segment's output columns and this segment only
        // re-projects; only push when that segment has no ordering/paging of its own to clobber.
        auto& producer = with_segments.back();
        if (producer->order_by.empty() && !producer->limit.has_value()) {
            producer->order_by = std::move(query.order_by);
            producer->limit = query.limit;
            query.order_by.clear();
            query.limit.reset();
            resolve_order_by_aliases(*producer);
        }
    }

    // openCypher WITH is not ISO GQL. RageDB is a pure GQL dialect: reject WITH with guidance toward
    // the linear-query equivalents (RETURN ... NEXT for a projection boundary, LET for a value binding,
    // FILTER for a post-projection predicate).
    if (check(TokenType::WITH)) {
        throw std::runtime_error(
            "WITH is not GQL: use 'RETURN ... NEXT' for a projection boundary, 'LET x = ...' for a "
            "value binding, or 'FILTER ...' for a post-projection predicate");
    }

    // Parse RETURN clause (optional if we performed writes, otherwise mandatory)
    if (match(TokenType::RETURN)) {
        if (match(TokenType::DISTINCT)) {
            query.distinct = true;
        }
        parse_return_items(query, false);

        // ISO GQL: `RETURN ... [ORDER BY ...] [LIMIT ...] NEXT` is an intermediate projection that
        // feeds the next linear-query statement -- the same pipeline boundary as openCypher WITH.
        // Detect it with a speculative scan past the optional result clauses so a final RETURN leaves
        // any trailing ORDER BY / LIMIT for the (union-aware) top-level parser.
        size_t saved_pos = pos;
        {
            GqlQuery scratch;
            parse_order_by(scratch);
            parse_limit(scratch);
        }
        bool intermediate = check(TokenType::NEXT_KW);
        pos = saved_pos;
        if (intermediate) {
            // A RETURN feeding NEXT projects columns that bind forward, so a non-variable projection
            // needs an alias to name its forwarded column (as the openCypher WITH form required).
            for (const auto& item : query.returns) {
                if (!item.alias && item.expr && item.expr->kind != ExpressionKind::VARIABLE) {
                    throw std::runtime_error("RETURN items before NEXT other than a plain variable must be aliased with AS");
                }
            }
            parse_order_by(query);
            resolve_order_by_aliases(query);
            parse_limit(query);
            consume(TokenType::NEXT_KW, "Expected 'NEXT'");
            // A FILTER/WHERE right after NEXT is a HAVING predicate on the projected rows.
            if (match(TokenType::FILTER_KW) || match(TokenType::WHERE)) {
                pending_having = parse_expression();
            }
            with_segments.push_back(std::make_shared<GqlQuery>(std::move(query)));
            continue;
        }
    } else {
        if (query.writes.empty()) {
            throw std::runtime_error("Query must contain either a RETURN clause or at least one write clause");
        }
    }

    query.with_segments = std::move(with_segments);
    return query;
    }  // end while(true) over pipeline segments
}

GqlQuery GqlParser::parse_query() {
    // 1. Detect optional EXPLAIN / PROFILE / NO_SEMANTIC prefix at query start
    bool explain = false;
    bool profile = false;
    bool skip_semantic = false;
    while (true) {
        if (match(TokenType::EXPLAIN)) {
            explain = true;
        } else if (match(TokenType::PROFILE)) {
            profile = true;
        } else if (match(TokenType::NO_SEMANTIC)) {
            skip_semantic = true;
        } else {
            break;
        }
    }

    // 2. Parse special CALL CLEAR CACHE utility command
    if (match(TokenType::CALL)) {
        consume(TokenType::CLEAR, "Expected 'CLEAR' after 'CALL'");
        consume(TokenType::CACHE, "Expected 'CACHE' after 'CLEAR'");
        GqlQuery query;
        query.clear_cache = true;
        query.explain = explain;
        query.profile = profile;
        query.skip_semantic = skip_semantic;
        return query;
    }

    // 3. Parse Schema Operations (CREATE/DROP/ALTER/SHOW)
    if (check(TokenType::CREATE) || check(TokenType::DROP) || check(TokenType::ALTER) || check(TokenType::SHOW)) {
        GqlQuery query;
        SchemaOperation schema;
        
        if (match(TokenType::CREATE)) {
            // CREATE VIEW
            if (check(TokenType::NAME) && (peek().text == "view" || peek().text == "VIEW")) {
                advance(); // consume "VIEW"
                std::string view_name = peek().text;
                consume(TokenType::NAME, "Expected view name identifier");
                
                // Consume optional or keyword 'AS'
                if (check(TokenType::NAME) && (peek().text == "AS" || peek().text == "as")) {
                    advance();
                } else {
                    consume(TokenType::AS, "Expected 'AS' keyword");
                }
                
                // Parse the view definition query and reconstruct its string representation from tokens
                std::string view_query_str;
                size_t start_pos = pos;
                GqlQuery view_query = parse_union();
                
                for (size_t i = start_pos; i < pos; ++i) {
                    if (!view_query_str.empty()) view_query_str += " ";
                    view_query_str += tokens[i].text;
                }
                
                schema.op = SchemaOperation::Op::CREATE_VIEW;
                schema.name = view_name;
                schema.query_string = view_query_str;
            // CREATE CONSTRAINT
            } else if (check(TokenType::NAME) && (peek().text == "CONSTRAINT" || peek().text == "constraint")) {
                advance(); // consume "CONSTRAINT"
                std::string constraint_name = peek().text;
                consume(TokenType::NAME, "Expected constraint name identifier");
                
                // Consume optional or keyword 'AS'
                if (check(TokenType::NAME) && (peek().text == "AS" || peek().text == "as")) {
                    advance();
                } else {
                    consume(TokenType::AS, "Expected 'AS' keyword");
                }
                
                // Parse the constraint query and reconstruct its string representation
                std::string constraint_query_str;
                size_t start_pos = pos;
                GqlQuery constraint_query = parse_union();
                
                for (size_t i = start_pos; i < pos; ++i) {
                    if (!constraint_query_str.empty()) constraint_query_str += " ";
                    constraint_query_str += tokens[i].text;
                }
                
                schema.op = SchemaOperation::Op::CREATE_CONSTRAINT;
                schema.name = constraint_name;
                schema.query_string = constraint_query_str;
            // CREATE FULLTEXT INDEX
            } else if (match(TokenType::FULLTEXT)) {
                consume(TokenType::INDEX, "Expected 'INDEX' after 'FULLTEXT'");
                std::string type_name = peek().text;
                consume(TokenType::NAME, "Expected type name identifier");
                consume(TokenType::DOT, "Expected '.'");
                std::string property_name = peek().text;
                consume(TokenType::NAME, "Expected property name identifier");
                schema.op = SchemaOperation::Op::CREATE_FULLTEXT_INDEX;
                schema.name = type_name;
                schema.alter_property_name = property_name;
            // CREATE INDEX (Standard property index)
            } else if (match(TokenType::INDEX)) {
                std::string type_name = peek().text;
                consume(TokenType::NAME, "Expected type name identifier");
                consume(TokenType::DOT, "Expected '.'");
                std::string property_name = peek().text;
                consume(TokenType::NAME, "Expected property name identifier");
                schema.op = SchemaOperation::Op::CREATE_INDEX;
                schema.name = type_name;
                schema.alter_property_name = property_name;
            } else {
                bool is_node = false;
                if (match(TokenType::NODE)) {
                    is_node = true;
                } else if (match(TokenType::RELATIONSHIP)) {
                    is_node = false;
                } else {
                    throw std::runtime_error("Expected 'NODE', 'RELATIONSHIP', 'REL', 'VIEW', 'CONSTRAINT', or 'INDEX' after 'CREATE'");
                }
                consume(TokenType::TYPE, "Expected 'TYPE' keyword");
                
                std::string type_name = peek().text;
                consume(TokenType::NAME, "Expected type name identifier");
                
                schema.op = is_node ? SchemaOperation::Op::CREATE_NODE_TYPE : SchemaOperation::Op::CREATE_REL_TYPE;
                schema.name = type_name;
                
                // Parse optional properties
                if (match(TokenType::LPAREN)) {
                    do {
                        std::string prop_name = peek().text;
                        consume(TokenType::NAME, "Expected property name identifier");
                        
                        std::string data_type;
                        if (match(TokenType::STRING_KW)) data_type = "string";
                        else if (match(TokenType::INTEGER_KW)) data_type = "integer";
                        else if (match(TokenType::DOUBLE_KW)) data_type = "double";
                        else if (match(TokenType::BOOLEAN_KW)) data_type = "boolean";
                        else if (match(TokenType::STRING_LIST_KW)) data_type = "string_list";
                        else if (match(TokenType::INTEGER_LIST_KW)) data_type = "integer_list";
                        else if (match(TokenType::DOUBLE_LIST_KW)) data_type = "double_list";
                        else if (match(TokenType::BOOLEAN_LIST_KW)) data_type = "boolean_list";
                        else {
                            throw std::runtime_error("Expected datatype (STRING, INTEGER, DOUBLE, BOOLEAN, or list variants) for property '" + prop_name + "'");
                        }
                        
                        schema.properties.push_back({prop_name, data_type});
                    } while (match(TokenType::COMMA));
                    consume(TokenType::RPAREN, "Expected ')' to close property list");
                }
            }
        }
        else if (match(TokenType::DROP)) {
            if (check(TokenType::NAME) && (peek().text == "VIEW" || peek().text == "view")) {
                advance(); // consume "VIEW"
                std::string view_name = peek().text;
                consume(TokenType::NAME, "Expected view name identifier");
                schema.op = SchemaOperation::Op::DROP_VIEW;
                schema.name = view_name;
            } else if (check(TokenType::NAME) && (peek().text == "CONSTRAINT" || peek().text == "constraint")) {
                advance(); // consume "CONSTRAINT"
                std::string constraint_name = peek().text;
                consume(TokenType::NAME, "Expected constraint name identifier");
                schema.op = SchemaOperation::Op::DROP_CONSTRAINT;
                schema.name = constraint_name;
            } else if (match(TokenType::INDEX)) {
                std::string type_name = peek().text;
                consume(TokenType::NAME, "Expected type name identifier");
                consume(TokenType::DOT, "Expected '.'");
                std::string property_name = peek().text;
                consume(TokenType::NAME, "Expected property name identifier");
                schema.op = SchemaOperation::Op::DROP_INDEX;
                schema.name = type_name;
                schema.alter_property_name = property_name;
            } else {
                bool is_node = false;
                if (match(TokenType::NODE)) {
                    is_node = true;
                } else if (match(TokenType::RELATIONSHIP)) {
                    is_node = false;
                } else {
                    throw std::runtime_error("Expected 'NODE', 'RELATIONSHIP', 'REL', 'VIEW', 'CONSTRAINT', or 'INDEX' after 'DROP'");
                }
                consume(TokenType::TYPE, "Expected 'TYPE' keyword");
                
                std::string type_name = peek().text;
                consume(TokenType::NAME, "Expected type name identifier");
                
                schema.op = is_node ? SchemaOperation::Op::DROP_NODE_TYPE : SchemaOperation::Op::DROP_REL_TYPE;
                schema.name = type_name;
            }
        }
        else if (match(TokenType::ALTER)) {
            bool is_node = false;
            if (match(TokenType::NODE)) {
                is_node = true;
            } else if (match(TokenType::RELATIONSHIP)) {
                is_node = false;
            } else {
                throw std::runtime_error("Expected 'NODE' or 'RELATIONSHIP' / 'REL' after 'ALTER'");
            }
            consume(TokenType::TYPE, "Expected 'TYPE' keyword");
            
            std::string type_name = peek().text;
            consume(TokenType::NAME, "Expected type name identifier");
            
            schema.op = is_node ? SchemaOperation::Op::ALTER_NODE_TYPE : SchemaOperation::Op::ALTER_REL_TYPE;
            schema.name = type_name;
            
            if (match(TokenType::ADD)) {
                schema.alter_op = SchemaOperation::AlterOp::ADD;
                std::string prop_name = peek().text;
                consume(TokenType::NAME, "Expected property name identifier");
                
                std::string data_type;
                if (match(TokenType::STRING_KW)) data_type = "string";
                else if (match(TokenType::INTEGER_KW)) data_type = "integer";
                else if (match(TokenType::DOUBLE_KW)) data_type = "double";
                else if (match(TokenType::BOOLEAN_KW)) data_type = "boolean";
                else if (match(TokenType::STRING_LIST_KW)) data_type = "string_list";
                else if (match(TokenType::INTEGER_LIST_KW)) data_type = "integer_list";
                else if (match(TokenType::DOUBLE_LIST_KW)) data_type = "double_list";
                else if (match(TokenType::BOOLEAN_LIST_KW)) data_type = "boolean_list";
                else {
                    throw std::runtime_error("Expected datatype for property '" + prop_name + "'");
                }
                schema.alter_property_name = prop_name;
                schema.alter_property_type = data_type;
            }
            else if (match(TokenType::DROP)) {
                schema.alter_op = SchemaOperation::AlterOp::DROP;
                std::string prop_name = peek().text;
                consume(TokenType::NAME, "Expected property name identifier");
                schema.alter_property_name = prop_name;
            }
            else {
                throw std::runtime_error("Expected 'ADD' or 'DROP' operation after type name in ALTER TYPE statement");
            }
        }
        else if (match(TokenType::SHOW)) {
            consume(TokenType::INDEXES, "Expected 'INDEXES' after 'SHOW'");
            schema.op = SchemaOperation::Op::SHOW_INDEXES;
            if (match(TokenType::ON)) {
                std::string type_name = peek().text;
                consume(TokenType::NAME, "Expected type name identifier after 'ON'");
                schema.name = type_name;
            } else {
                schema.name = "";
            }
        }
        
        query.schema_op = std::move(schema);
        query.explain = explain;
        query.profile = profile;
        query.skip_semantic = skip_semantic;
        consume(TokenType::EOF_TOK, "Expected end of query");
        return query;
    }

    GqlQuery query = parse_union();

    // Parse optional top-level ORDER BY / LIMIT clauses
    parse_order_by(query);
    if (query.kind == QueryKind::SINGLE) {
        resolve_order_by_aliases(query);
    }
    parse_limit(query);

    query.explain = explain;
    query.profile = profile;
    query.skip_semantic = skip_semantic;

    consume(TokenType::EOF_TOK, "Expected end of query");
    return query;
}

GqlQuery GqlParser::parse_union() {
    // UNION and EXCEPT share precedence (both bind looser than INTERSECT), so a single left-to-right loop
    // handles them. Each accepts the ISO GQL set quantifier ALL | DISTINCT; DISTINCT is the default, so a
    // bare UNION and UNION DISTINCT mean the same thing.
    GqlQuery query = parse_intersect();
    while (check(TokenType::UNION) || check(TokenType::EXCEPT)) {
        const bool is_except = check(TokenType::EXCEPT);
        advance();
        bool all = false;
        if (match(TokenType::ALL_KW)) {
            all = true;
        } else {
            match(TokenType::DISTINCT);   // optional explicit DISTINCT, the default
        }
        GqlQuery right = parse_intersect();

        GqlQuery combined;
        if (is_except) {
            combined.kind = all ? QueryKind::EXCEPT_ALL : QueryKind::EXCEPT;
        } else {
            combined.kind = all ? QueryKind::UNION_ALL : QueryKind::UNION;
        }
        combined.left = std::make_unique<GqlQuery>(std::move(query));
        combined.right = std::make_unique<GqlQuery>(std::move(right));
        query = std::move(combined);
    }
    return query;
}

GqlQuery GqlParser::parse_intersect() {
    GqlQuery query = parse_single_query();
    while (match(TokenType::INTERSECT)) {
        bool all = false;
        if (match(TokenType::ALL_KW)) {
            all = true;
        } else {
            match(TokenType::DISTINCT);   // optional explicit DISTINCT, the default
        }
        GqlQuery right = parse_single_query();

        GqlQuery combined;
        combined.kind = all ? QueryKind::INTERSECT_ALL : QueryKind::INTERSECT;
        combined.left = std::make_unique<GqlQuery>(std::move(query));
        combined.right = std::make_unique<GqlQuery>(std::move(right));
        query = std::move(combined);
    }
    return query;
}

void GqlParser::parse_edge_details(PatternEdge& edge) {
    if (check(TokenType::NAME)) {
        edge.variable = peek().text;
        advance();
    }
    if (match(TokenType::COLON) || match(TokenType::IS)) {
        edge.label_expr = parse_label_expression();
    }
    if (match(TokenType::STAR)) {
        edge.is_variable_length = true;
        edge.min_hops = 1;
        edge.max_hops = std::numeric_limits<uint64_t>::max();
        if (check(TokenType::NUMBER)) {
            uint64_t val = peek().int_value;
            advance();
            if (match(TokenType::DOT)) {
                consume(TokenType::DOT, "Expected '..' for repetition range");
                edge.min_hops = val;
                if (check(TokenType::NUMBER)) {
                    edge.max_hops = peek().int_value;
                    advance();
                }
            } else {
                edge.min_hops = val;
                edge.max_hops = val;
            }
        } else if (match(TokenType::DOT)) {
            consume(TokenType::DOT, "Expected '..' for repetition range");
            edge.min_hops = 1;
            if (check(TokenType::NUMBER)) {
                edge.max_hops = peek().int_value;
                advance();
            }
        }
    }
    if (check(TokenType::LBRACE)) {
        edge.properties = parse_properties(&edge.property_exprs);
    }
    if (match(TokenType::WHERE)) {
        edge.where_expr = parse_expression();
    }
    if (match(TokenType::COST_KW)) {
        edge.cost_expr = parse_expression();
    }
    if (edge.cost_expr && (!edge.properties.empty() || edge.where_expr)) {
        throw std::runtime_error("COST cannot be used with property specification or inline WHERE within the same edge pattern");
    }
}

/**
 * @brief Parses a path pattern consisting of alternating nodes and edges.
 * 
 * Path patterns are defined as node-edge-node structures.
 * 
 * @return PathPattern The parsed path pattern AST structure.
 */
PathPattern GqlParser::parse_path_pattern() {
    bool is_parenthesized = false;
    if (check(TokenType::LPAREN) && peek(1).type == TokenType::LPAREN) {
        consume(TokenType::LPAREN, "Expected '('");
        is_parenthesized = true;
    }

    PathPattern pattern;
    pattern.nodes.push_back(parse_node_pattern());

    while (check(TokenType::RIGHT_ARROW) || check(TokenType::LEFT_ARROW) ||
           check(TokenType::DASH_LB) || check(TokenType::LT_DASH_LB) ||
           check(TokenType::MINUS)) {

        PatternEdge edge;
        if (match(TokenType::RIGHT_ARROW)) {
            edge.direction = EdgeDirection::RIGHT;
        } else if (match(TokenType::LEFT_ARROW)) {
            edge.direction = EdgeDirection::LEFT;
        } else if (match(TokenType::MINUS)) {
            edge.direction = EdgeDirection::ANY;
        } else if (match(TokenType::DASH_LB)) {
            // Outgoing or Undirected Edge with detail: -[e:L {p:v}]-> or -[e:L {p:v}]-
            parse_edge_details(edge);
            if (match(TokenType::RB_DASH_GT)) {
                edge.direction = EdgeDirection::RIGHT;
            } else {
                consume(TokenType::RB_DASH, "Expected ']-' or ']->' to end relationship description");
                edge.direction = EdgeDirection::ANY;
            }
            // Parse repetition suffix if present (e.g. -[e]->+ or -[e]->* or -[e]->{1, 5})
            if (match(TokenType::PLUS)) {
                // '+' denotes 1 or more hops
                edge.is_variable_length = true;
                edge.min_hops = 1;
                edge.max_hops = std::numeric_limits<uint64_t>::max();
            } else if (match(TokenType::STAR)) {
                // '*' denotes 0 or more hops
                edge.is_variable_length = true;
                edge.min_hops = 0;
                edge.max_hops = std::numeric_limits<uint64_t>::max();
            } else if (match(TokenType::LBRACE)) {
                edge.is_variable_length = true;
                if (check(TokenType::NUMBER)) {
                    uint64_t val = peek().int_value;
                    consume(TokenType::NUMBER, "Expected number");
                    if (match(TokenType::COMMA)) {
                        edge.min_hops = val;
                        if (check(TokenType::NUMBER)) {
                            edge.max_hops = peek().int_value;
                            consume(TokenType::NUMBER, "Expected number");
                        } else {
                            edge.max_hops = std::numeric_limits<uint64_t>::max();
                        }
                    } else {
                        edge.min_hops = val;
                        edge.max_hops = val;
                    }
                } else if (match(TokenType::COMMA)) {
                    edge.min_hops = 0;
                    edge.max_hops = peek().int_value;
                    consume(TokenType::NUMBER, "Expected maximum hops");
                } else {
                    throw std::runtime_error("Invalid quantifier format inside '{}'");
                }
                consume(TokenType::RBRACE, "Expected '}'");
            }
        } else if (match(TokenType::LT_DASH_LB)) {
            // Incoming Edge with detail: <-[e:L {p:v}]-
            parse_edge_details(edge);
            consume(TokenType::RB_DASH, "Expected ']-' to end incoming relationship description");
            edge.direction = EdgeDirection::LEFT;
            // Parse repetition suffix if present (e.g. <-[e]-+ or <-[e]-* or <-[e]-{1, 5})
            if (match(TokenType::PLUS)) {
                // '+' denotes 1 or more hops
                edge.is_variable_length = true;
                edge.min_hops = 1;
                edge.max_hops = std::numeric_limits<uint64_t>::max();
            } else if (match(TokenType::STAR)) {
                // '*' denotes 0 or more hops
                edge.is_variable_length = true;
                edge.min_hops = 0;
                edge.max_hops = std::numeric_limits<uint64_t>::max();
            } else if (match(TokenType::LBRACE)) {
                edge.is_variable_length = true;
                if (check(TokenType::NUMBER)) {
                    uint64_t val = peek().int_value;
                    consume(TokenType::NUMBER, "Expected number");
                    if (match(TokenType::COMMA)) {
                        edge.min_hops = val;
                        if (check(TokenType::NUMBER)) {
                            edge.max_hops = peek().int_value;
                            consume(TokenType::NUMBER, "Expected number");
                        } else {
                            edge.max_hops = std::numeric_limits<uint64_t>::max();
                        }
                    } else {
                        edge.min_hops = val;
                        edge.max_hops = val;
                    }
                } else if (match(TokenType::COMMA)) {
                    edge.min_hops = 0;
                    edge.max_hops = peek().int_value;
                    consume(TokenType::NUMBER, "Expected maximum hops");
                } else {
                    throw std::runtime_error("Invalid quantifier format inside '{}'");
                }
                consume(TokenType::RBRACE, "Expected '}'");
            }
        }

        pattern.edges.push_back(edge);
        pattern.nodes.push_back(parse_node_pattern());
    }

    if (is_parenthesized) {
        consume(TokenType::RPAREN, "Expected ')'");
        if (match(TokenType::QUESTION)) {
            pattern.is_questioned = true;
        } else if (match(TokenType::LBRACE)) {
            uint64_t min_hops = 1;
            uint64_t max_hops = 1;
            bool is_range = false;

            if (check(TokenType::NUMBER)) {
                uint64_t val = peek().int_value;
                consume(TokenType::NUMBER, "Expected number");
                if (match(TokenType::COMMA)) {
                    min_hops = val;
                    is_range = true;
                    if (check(TokenType::NUMBER)) {
                        max_hops = peek().int_value;
                        consume(TokenType::NUMBER, "Expected number");
                    } else {
                        max_hops = std::numeric_limits<uint64_t>::max();
                    }
                } else {
                    min_hops = val;
                    max_hops = val;
                }
            } else if (match(TokenType::COMMA)) {
                min_hops = 0;
                max_hops = peek().int_value;
                consume(TokenType::NUMBER, "Expected maximum hops");
                is_range = true;
            } else {
                throw std::runtime_error("Invalid quantifier format inside '{}' after parenthesized path group");
            }
            consume(TokenType::RBRACE, "Expected '}'");

            if (pattern.edges.size() == 1) {
                pattern.edges[0].is_variable_length = true;
                pattern.edges[0].min_hops = min_hops;
                pattern.edges[0].max_hops = max_hops;
            } else if (pattern.edges.size() > 1) {
                if (is_range) {
                    throw std::runtime_error("Range repetitions on multi-edge parenthesized path groups are not supported");
                }
                uint64_t k = min_hops;
                if (k == 0) {
                    pattern.edges.clear();
                    pattern.nodes.resize(1);
                } else if (k > 1) {
                    std::vector<PatternNode> base_nodes = pattern.nodes;
                    std::vector<PatternEdge> base_edges = pattern.edges;
                    size_t L = base_edges.size();

                    for (uint64_t iteration = 1; iteration < k; ++iteration) {
                        for (size_t j = 0; j < L; ++j) {
                            PatternEdge new_edge = base_edges[j];
                            if (!new_edge.variable.empty()) {
                                new_edge.variable += "_" + std::to_string(iteration);
                            }
                            pattern.edges.push_back(std::move(new_edge));

                            PatternNode new_node = base_nodes[j + 1];
                            if (!new_node.variable.empty()) {
                                new_node.variable += "_" + std::to_string(iteration);
                            }
                            pattern.nodes.push_back(std::move(new_node));
                        }
                    }
                }
            }
        }
    }

    return pattern;
}

/**
 * @brief Parses a node pattern of the form: (variable:Label {prop1: val1, ...})
 * 
 * @return PatternNode The parsed node pattern AST structure.
 */
PatternNode GqlParser::parse_node_pattern() {
    PatternNode node;
    consume(TokenType::LPAREN, "Expected '(' to start node pattern");

    if (check(TokenType::NAME)) {
        node.variable = peek().text;
        advance();
    }
    if (match(TokenType::COLON) || match(TokenType::IS)) {
        node.label_expr = parse_label_expression();
    }
    if (check(TokenType::LBRACE)) {
        node.properties = parse_properties(&node.property_exprs);
    }
    if (match(TokenType::WHERE)) {
        node.where_expr = parse_expression();
    }

    consume(TokenType::RPAREN, "Expected ')' to end node pattern");
    return node;
}

/**
 * @brief Parses property maps in nodes or edges: {name: 'John', age: 30}
 * 
 * @return std::map<std::string, property_type_t> The map of parsed keys to typed literal values.
 */
std::map<std::string, property_type_t> GqlParser::parse_properties(
        std::map<std::string, std::shared_ptr<Expression>>* property_exprs) {
    std::map<std::string, property_type_t> props;
    consume(TokenType::LBRACE, "Expected '{' to start property map");
    if (!check(TokenType::RBRACE)) {
        do {
            std::string key = peek().text;
            consume(TokenType::NAME, "Expected property key name");
            consume(TokenType::COLON, "Expected ':' after property key");

            property_type_t value;
            bool is_negative = match(TokenType::MINUS);
            if (check(TokenType::NUMBER)) {
                value = is_negative ? -peek().int_value : peek().int_value;
                advance();
            } else if (check(TokenType::FLOAT_LIT)) {
                value = is_negative ? -peek().float_value : peek().float_value;
                advance();
            } else if (!is_negative && match(TokenType::TRUE_KW)) {
                value = true;
            } else if (!is_negative && match(TokenType::FALSE_KW)) {
                value = false;
            } else if (!is_negative && match(TokenType::NULL_KW)) {
                value = std::monostate{};
            } else if (!is_negative && check(TokenType::STRING_LIT)) {
                value = peek().text;
                advance();
            } else if (property_exprs) {
                // A property map value is a general value expression, not only a literal: a variable
                // bound by an earlier segment, a LET binding or any computed expression is legal here
                // and is resolved against the current row before the node is looked up. Step back over
                // a consumed '-' so the expression parser sees the whole term.
                if (is_negative) {
                    --pos;
                }
                (*property_exprs)[key] = parse_expression();
                continue;
            } else {
                throw std::runtime_error("Expected literal value for property map");
            }

            props[key] = value;
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RBRACE, "Expected '}' to end property map");
    return props;
}

/**
 * @brief Helper rule to initiate precedence climbing/recursive descent for expressions.
 * 
 * @return std::unique_ptr<Expression> The root node of the parsed expression AST.
 */
std::unique_ptr<Expression> GqlParser::parse_expression() {
    return parse_or();
}

/**
 * @brief Parses boolean OR expressions.
 * 
 * Precedence level: lowest.
 * 
 * @return std::unique_ptr<Expression> Parsed expression node.
 */
std::unique_ptr<Expression> GqlParser::parse_or() {
    auto expr = parse_and();
    while (match(TokenType::OR)) {
        auto right = parse_and();
        expr = std::make_unique<BinaryOpExpr>(BinaryOpKind::OR, std::move(expr), std::move(right));
    }
    return expr;
}

/**
 * @brief Parses boolean AND expressions.
 * 
 * @return std::unique_ptr<Expression> Parsed expression node.
 */
std::unique_ptr<Expression> GqlParser::parse_and() {
    auto expr = parse_comparison();
    while (match(TokenType::AND)) {
        auto right = parse_comparison();
        expr = std::make_unique<BinaryOpExpr>(BinaryOpKind::AND, std::move(expr), std::move(right));
    }
    return expr;
}

/**
 * @brief Parses comparison expressions: =, <>, <, <=, >, >=
 * 
 * @return std::unique_ptr<Expression> Parsed expression node.
 */
std::unique_ptr<Expression> GqlParser::parse_comparison() {
    auto expr = parse_add_sub();
    while (true) {
        if (match(TokenType::IS)) {
            bool is_not = false;
            if (match(TokenType::NOT)) {
                is_not = true;
            }
            // `x IS [NOT] LABELED <labelExpression>` -- the label side is the same grammar as a pattern's,
            // so AND/OR/NOT compose there exactly as they do inside a pattern.
            if (match(TokenType::LABELED)) {
                // Symbolic label operators only: a trailing `OR`/`AND` here belongs to the enclosing
                // boolean expression, not to the label.
                expr = std::make_unique<IsLabeledExpr>(std::move(expr), parse_label_expression(false), is_not);
                continue;
            }
            // `e IS [NOT] DIRECTED` and `n IS [NOT] (SOURCE | DESTINATION) OF e`. DIRECTED/SOURCE/
            // DESTINATION/OF are not reserved words, so they arrive as identifiers here.
            if (check(TokenType::NAME)) {
                std::string kw = peek().text;
                std::transform(kw.begin(), kw.end(), kw.begin(), [](unsigned char c){ return std::toupper(c); });
                if (kw == "DIRECTED") {
                    advance();
                    expr = std::make_unique<IsDirectedExpr>(std::move(expr), is_not);
                    continue;
                }
                if (kw == "SOURCE" || kw == "DESTINATION") {
                    bool is_source = (kw == "SOURCE");
                    advance();
                    std::string of_tok = check(TokenType::NAME) ? peek().text : "";
                    std::transform(of_tok.begin(), of_tok.end(), of_tok.begin(), [](unsigned char c){ return std::toupper(c); });
                    if (of_tok != "OF") {
                        throw std::runtime_error("Expected 'OF' after IS [NOT] SOURCE/DESTINATION");
                    }
                    advance(); // consume OF
                    if (!check(TokenType::NAME)) {
                        throw std::runtime_error("Expected a relationship variable after 'OF'");
                    }
                    std::string edge_var = peek().text;
                    advance();
                    expr = std::make_unique<IsSourceDestExpr>(std::move(expr),
                        std::make_unique<VariableExpr>(edge_var), is_source, is_not);
                    continue;
                }
            }
            consume(TokenType::NULL_KW, "Expected 'NULL' or 'LABELED' after 'IS [NOT]'");
            expr = std::make_unique<IsNullExpr>(std::move(expr), is_not);
            continue;
        }

        // x IN [a, b, ...] (a literal list) desugars to (x = a OR x = b OR ...), reusing tested EQ/OR
        // execution; an empty list is always false. x IN <expr> (a list-valued right operand, e.g. a
        // collect_list result) becomes an InExpr evaluated as runtime membership.
        if (match(TokenType::IN_KW)) {
            if (check(TokenType::LBRACKET)) {
                advance(); // consume '['
                std::vector<std::unique_ptr<Expression>> items;
                if (!check(TokenType::RBRACKET)) {
                    do {
                        items.push_back(parse_expression());
                    } while (match(TokenType::COMMA));
                }
                consume(TokenType::RBRACKET, "Expected ']' to close 'IN' list");
                if (items.empty()) {
                    expr = std::make_unique<LiteralExpr>(false);
                } else {
                    std::unique_ptr<Expression> disjunction;
                    for (auto& item : items) {
                        auto eq = std::make_unique<BinaryOpExpr>(BinaryOpKind::EQ, expr->clone(), std::move(item));
                        disjunction = disjunction
                            ? std::make_unique<BinaryOpExpr>(BinaryOpKind::OR, std::move(disjunction), std::move(eq))
                            : std::move(eq);
                    }
                    expr = std::move(disjunction);
                }
            } else {
                auto list_expr = parse_add_sub();
                expr = std::make_unique<InExpr>(std::move(expr), std::move(list_expr));
            }
            continue;
        }

        if (check(TokenType::EQ) || check(TokenType::NE) ||
            check(TokenType::LT) || check(TokenType::LE) ||
            check(TokenType::GT) || check(TokenType::GE) ||
            check(TokenType::STARTS_WITH) || check(TokenType::ENDS_WITH) ||
            check(TokenType::CONTAINS)) {
            TokenType op_type = peek().type;
            advance();

            BinaryOpKind op;
            if (op_type == TokenType::EQ) op = BinaryOpKind::EQ;
            else if (op_type == TokenType::NE) op = BinaryOpKind::NE;
            else if (op_type == TokenType::LT) op = BinaryOpKind::LT;
            else if (op_type == TokenType::LE) op = BinaryOpKind::LE;
            else if (op_type == TokenType::GT) op = BinaryOpKind::GT;
            else if (op_type == TokenType::GE) op = BinaryOpKind::GE;
            else if (op_type == TokenType::STARTS_WITH) op = BinaryOpKind::STARTS_WITH;
            else if (op_type == TokenType::ENDS_WITH) op = BinaryOpKind::ENDS_WITH;
            else op = BinaryOpKind::CONTAINS;

            auto right = parse_add_sub();
            expr = std::make_unique<BinaryOpExpr>(op, std::move(expr), std::move(right));
            continue;
        }

        break;
    }
    return expr;
}

/**
 * @brief Parses additive arithmetic expressions: +, -, and ||
 * 
 * @return std::unique_ptr<Expression> Parsed expression node.
 */
std::unique_ptr<Expression> GqlParser::parse_add_sub() {
    auto expr = parse_mul_div();
    while (check(TokenType::PLUS) || check(TokenType::MINUS) || check(TokenType::PIPE_PIPE)) {
        TokenType op_type = peek().type;
        advance();
        BinaryOpKind op;
        if (op_type == TokenType::PLUS) op = BinaryOpKind::ADD;
        else if (op_type == TokenType::MINUS) op = BinaryOpKind::SUB;
        else op = BinaryOpKind::CONCAT;
        auto right = parse_mul_div();
        expr = std::make_unique<BinaryOpExpr>(op, std::move(expr), std::move(right));
    }
    return expr;
}

/**
 * @brief Parses multiplicative arithmetic expressions: * and /
 * 
 * @return std::unique_ptr<Expression> Parsed expression node.
 */
std::unique_ptr<Expression> GqlParser::parse_mul_div() {
    auto expr = parse_unary();
    while (check(TokenType::STAR) || check(TokenType::SLASH)) {
        TokenType op_type = peek().type;
        advance();
        BinaryOpKind op = (op_type == TokenType::STAR) ? BinaryOpKind::MUL : BinaryOpKind::DIV;
        auto right = parse_unary();
        expr = std::make_unique<BinaryOpExpr>(op, std::move(expr), std::move(right));
    }
    return expr;
}

/**
 * @brief Parses unary prefix expressions: NOT and - (negative)
 * 
 * @return std::unique_ptr<Expression> Parsed expression node.
 */
std::unique_ptr<Expression> GqlParser::parse_unary() {
    if (match(TokenType::NOT)) {
        auto right = parse_unary();
        return std::make_unique<UnaryOpExpr>(UnaryOpKind::NOT, std::move(right));
    }
    if (match(TokenType::MINUS)) {
        auto right = parse_unary();
        return std::make_unique<UnaryOpExpr>(UnaryOpKind::NEG, std::move(right));
    }
    return parse_primary();
}

/**
 * @brief Parses primary expressions: literals, variables, property lookups, and parenthesized sub-expressions.
 * 
 * Precedence level: highest.
 * 
 * @return std::unique_ptr<Expression> Parsed expression node.
 * @throws std::runtime_error If an unexpected token or syntax issue is encountered.
 */
void GqlParser::parse_braced_subquery(std::vector<MatchStatement>& matches,
                                      std::unique_ptr<Expression>& sub_where) {
    consume(TokenType::LBRACE, "Expected '{' to start subquery");
    int sub_match_id = 0;
    {
        while (check(TokenType::MATCH) || (check(TokenType::OPTIONAL) && peek(1).type == TokenType::MATCH)) {
            MatchStatement stmt;
            if (match(TokenType::OPTIONAL)) {
                stmt.is_optional = true;
            }
            consume(TokenType::MATCH, "Expected MATCH");
            if (check(TokenType::NAME) && peek(1).type == TokenType::IN_KW) {
                stmt.is_search = true;
                stmt.search_var = peek().text;
                consume(TokenType::NAME, "Expected variable name before 'IN'");
                consume(TokenType::IN_KW, "Expected 'IN'");
                consume(TokenType::SEARCH, "Expected 'SEARCH'");
                
                if (match(TokenType::LPAREN)) {
                    do {
                        std::string type_name = peek().text;
                        consume(TokenType::NAME, "Expected type name");
                        consume(TokenType::DOT, "Expected '.'");
                        std::string prop_name = peek().text;
                        consume(TokenType::NAME, "Expected property name");
                        
                        stmt.search_type = type_name;
                        stmt.search_properties.push_back(prop_name);
                    } while (match(TokenType::COMMA));
                    consume(TokenType::RPAREN, "Expected ')' after property list");
                } else {
                    std::string type_name = peek().text;
                    consume(TokenType::NAME, "Expected type name");
                    consume(TokenType::DOT, "Expected '.'");
                    std::string prop_name = peek().text;
                    consume(TokenType::NAME, "Expected property name");
                    
                    stmt.search_type = type_name;
                    stmt.search_properties.push_back(prop_name);
                }
                
                consume(TokenType::FOR, "Expected 'FOR'");
                stmt.search_string = peek().text;
                consume(TokenType::STRING_LIT, "Expected query string literal after 'FOR'");
                
                if (match(TokenType::OPTIONS)) {
                    consume(TokenType::LBRACE, "Expected '{' after OPTIONS");
                    if (!check(TokenType::RBRACE)) {
                        do {
                            std::string opt_key = peek().text;
                            consume(TokenType::NAME, "Expected option key identifier");
                            consume(TokenType::COLON, "Expected ':' after option key");
                            
                            std::string opt_val;
                            if (check(TokenType::STRING_LIT)) {
                                opt_val = peek().text;
                                consume(TokenType::STRING_LIT, "Expected string literal");
                            } else if (check(TokenType::TRUE_KW)) {
                                opt_val = "true";
                                consume(TokenType::TRUE_KW, "Expected true");
                            } else if (check(TokenType::FALSE_KW)) {
                                opt_val = "false";
                                consume(TokenType::FALSE_KW, "Expected false");
                            } else if (check(TokenType::NAME)) {
                                opt_val = peek().text;
                                consume(TokenType::NAME, "Expected name identifier");
                            } else {
                                throw std::runtime_error("Expected option value");
                            }
                            stmt.search_options[opt_key] = opt_val;
                        } while (match(TokenType::COMMA));
                    }
                    consume(TokenType::RBRACE, "Expected '}' to close OPTIONS");
                }
                
                consume(TokenType::YIELD, "Expected 'YIELD'");
                stmt.yield_var = peek().text;
                consume(TokenType::NAME, "Expected variable name to bind search result");
                consume(TokenType::COMMA, "Expected ','");
                stmt.yield_score_var = peek().text;
                consume(TokenType::NAME, "Expected score variable name");
            } else {
                stmt.pattern = parse_path_pattern();
            }
            stmt.id = sub_match_id++;
            matches.push_back(std::move(stmt));
        }
        // openCypher-style bare pattern subquery, e.g. EXISTS { (a)-[:KNOWS]-(b) }: treat the
        // pattern as an implicit MATCH (this is the form the LDBC queries use).
        if (matches.empty() && !check(TokenType::RBRACE) && !check(TokenType::WHERE)) {
            MatchStatement stmt;
            stmt.pattern = parse_path_pattern();
            stmt.id = sub_match_id++;
            matches.push_back(std::move(stmt));
        }
    }
    sub_where = nullptr;
    if (match(TokenType::WHERE)) {
        sub_where = parse_expression();
    }
    consume(TokenType::RBRACE, "Expected '}' after subquery");
}

std::unique_ptr<Expression> GqlParser::parse_primary() {
    if (match(TokenType::EXISTS)) {
        std::vector<MatchStatement> matches;
        std::unique_ptr<Expression> sub_where;
        parse_braced_subquery(matches, sub_where);
        return std::make_unique<ExistsExpr>(std::move(matches), std::move(sub_where));
    }
    // List literal: [a, b, c]. (The `x IN [a, b]` form is desugared to OR earlier, so it never reaches
    // here; this is the standalone list value -- what FOR expands and what a list-typed binding holds.)
    if (match(TokenType::LBRACKET)) {
        std::vector<std::unique_ptr<Expression>> elements;
        if (!check(TokenType::RBRACKET)) {
            do {
                elements.push_back(parse_expression());
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACKET, "Expected ']' to close list literal");
        return std::make_unique<ListExpr>(std::move(elements));
    }
    // CAST(<expr> AS <type>). The primitive type names are lexed as their own keywords (they are also
    // schema-DDL types), so accept those tokens as well as the spellings that arrive as plain names.
    if (match(TokenType::CAST)) {
        consume(TokenType::LPAREN, "Expected '(' after CAST");
        auto value = parse_expression();
        consume(TokenType::AS, "Expected 'AS' in CAST");

        std::optional<CastType> target;
        if (match(TokenType::STRING_KW)) {
            target = CastType::STRING;
        } else if (match(TokenType::INTEGER_KW)) {
            target = CastType::INTEGER;
        } else if (match(TokenType::DOUBLE_KW)) {
            target = CastType::FLOAT;
        } else if (match(TokenType::BOOLEAN_KW)) {
            target = CastType::BOOLEAN;
        } else if (check(TokenType::NAME)) {
            std::string type_name = peek().text;
            advance();
            std::transform(type_name.begin(), type_name.end(), type_name.begin(),
                           [](unsigned char c) { return std::toupper(c); });
            if (type_name == "VARCHAR" || type_name == "TEXT") {
                target = CastType::STRING;
            } else if (type_name == "BIGINT") {
                target = CastType::INTEGER;
            } else if (type_name == "FLOAT" || type_name == "REAL") {
                target = CastType::FLOAT;
            } else {
                throw std::runtime_error("Unsupported CAST target type: " + type_name);
            }
        } else {
            throw std::runtime_error("Expected a type name after 'AS' in CAST");
        }

        consume(TokenType::RPAREN, "Expected ')' to close CAST");
        return std::make_unique<CastExpr>(std::move(value), *target);
    }
    // ISO GQL COUNT { <pattern subquery> } counts subquery matches (distinct from the count(x)
    // aggregate). It shares the braced-subquery body with EXISTS and reuses SizeExpr (a match count).
    if (check(TokenType::NAME) && peek(1).type == TokenType::LBRACE) {
        std::string upper_name = peek().text;
        std::transform(upper_name.begin(), upper_name.end(), upper_name.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        if (upper_name == "COUNT") {
            advance(); // consume COUNT
            std::vector<MatchStatement> matches;
            std::unique_ptr<Expression> sub_where;
            parse_braced_subquery(matches, sub_where);
            return std::make_unique<SizeExpr>(std::move(matches), std::move(sub_where));
        }
    }
    // Searched CASE expression: CASE WHEN cond THEN val [WHEN ...] [ELSE val] END.
    if (match(TokenType::CASE_KW)) {
        std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> branches;
        while (match(TokenType::WHEN_KW)) {
            auto cond = parse_expression();
            consume(TokenType::THEN_KW, "Expected 'THEN' after CASE WHEN condition");
            auto val = parse_expression();
            branches.emplace_back(std::move(cond), std::move(val));
        }
        if (branches.empty()) {
            throw std::runtime_error("CASE expression requires at least one WHEN branch");
        }
        std::unique_ptr<Expression> else_expr = nullptr;
        if (match(TokenType::ELSE_KW)) {
            else_expr = parse_expression();
        }
        consume(TokenType::END_KW, "Expected 'END' to close CASE expression");
        return std::make_unique<CaseExpr>(std::move(branches), std::move(else_expr));
    }
    if (match(TokenType::TRUE_KW)) {
        return std::make_unique<LiteralExpr>(true);
    }
    if (match(TokenType::FALSE_KW)) {
        return std::make_unique<LiteralExpr>(false);
    }
    if (match(TokenType::NULL_KW)) {
        return std::make_unique<LiteralExpr>(std::monostate{});
    }
    if (check(TokenType::NUMBER)) {
        int64_t val = peek().int_value;
        advance();
        return std::make_unique<LiteralExpr>(val);
    }
    if (check(TokenType::FLOAT_LIT)) {
        double val = peek().float_value;
        advance();
        return std::make_unique<LiteralExpr>(val);
    }
    if (check(TokenType::STRING_LIT)) {
        std::string val = peek().text;
        advance();
        return std::make_unique<LiteralExpr>(val);
    }
    if (match(TokenType::LPAREN)) {
        auto expr = parse_expression();
        consume(TokenType::RPAREN, "Expected ')' after expression");
        return expr;
    }
    if (check(TokenType::NAME)) {
        std::string var = peek().text;
        // Check for aggregate function call: NAME '('
        if (peek(1).type == TokenType::LPAREN) {
            std::string upper_name = var;
            std::transform(upper_name.begin(), upper_name.end(), upper_name.begin(), [](unsigned char c){ return std::toupper(c); });
            // `collect(x)` is the openCypher spelling; ISO GQL's general set function is `collect_list(x)`.
            // The dialect is pure GQL (WITH was removed for the same reason), so reject it rather than
            // quietly accepting a Cypher-ism.
            if (upper_name == "COLLECT") {
                throw std::runtime_error(
                    "collect is not GQL: use collect_list(...)");
            }
            if (upper_name == "COUNT" || upper_name == "SUM" || upper_name == "AVG" || upper_name == "MIN" || upper_name == "MAX" ||
                upper_name == "COLLECT_LIST" || upper_name == "STDDEV_POP" || upper_name == "STDDEV_SAMP") {
                advance(); // consume function name
                consume(TokenType::LPAREN, "Expected '(' after aggregate function");
                
                std::unique_ptr<Expression> arg = nullptr;
                bool distinct = false;
                // Special case for count(*)
                if (upper_name == "COUNT" && check(TokenType::STAR)) {
                    advance(); // consume '*'
                } else {
                    // Optional DISTINCT before the aggregated expression, e.g. count(DISTINCT x).
                    if (match(TokenType::DISTINCT)) {
                        distinct = true;
                    }
                    arg = parse_expression();
                }
                consume(TokenType::RPAREN, "Expected ')' after aggregate function argument");

                AggregateKind fn;
                if (upper_name == "COUNT") fn = AggregateKind::COUNT;
                else if (upper_name == "SUM") fn = AggregateKind::SUM;
                else if (upper_name == "AVG") fn = AggregateKind::AVG;
                else if (upper_name == "MIN") fn = AggregateKind::MIN;
                else if (upper_name == "COLLECT_LIST") fn = AggregateKind::COLLECT;
                else if (upper_name == "STDDEV_POP") fn = AggregateKind::STDDEV_POP;
                else if (upper_name == "STDDEV_SAMP") fn = AggregateKind::STDDEV_SAMP;
                else fn = AggregateKind::MAX;

                return std::make_unique<AggregateExpr>(fn, std::move(arg), distinct);
            } else if (upper_name == "PERCENTILE_CONT" || upper_name == "PERCENTILE_DISC") {
                // ISO GQL binary set function: PERCENTILE_CONT(value, fraction). The second argument
                // is the percentile in [0,1]; the first is the value expression to rank.
                advance(); // consume function name
                consume(TokenType::LPAREN, "Expected '(' after percentile function");
                bool distinct = match(TokenType::DISTINCT);
                std::unique_ptr<Expression> value = parse_expression();
                consume(TokenType::COMMA, "Expected ',' between percentile value and fraction");
                std::unique_ptr<Expression> fraction = parse_expression();
                consume(TokenType::RPAREN, "Expected ')' after percentile arguments");
                AggregateKind fn = upper_name == "PERCENTILE_CONT"
                    ? AggregateKind::PERCENTILE_CONT : AggregateKind::PERCENTILE_DISC;
                auto agg = std::make_unique<AggregateExpr>(fn, std::move(value), distinct);
                agg->arg2 = std::move(fraction);
                return agg;
            } else if (upper_name == "SIZE") {
                advance(); // consume "size"
                consume(TokenType::LPAREN, "Expected '(' after size");
                
                std::vector<MatchStatement> matches;
                MatchStatement stmt;
                stmt.pattern = parse_path_pattern();
                stmt.id = 0;
                matches.push_back(std::move(stmt));
                
                std::unique_ptr<Expression> sub_where = nullptr;
                if (match(TokenType::WHERE)) {
                    sub_where = parse_expression();
                }
                
                consume(TokenType::RPAREN, "Expected ')' after size expression");
                return std::make_unique<SizeExpr>(std::move(matches), std::move(sub_where));
            } else {
                // Generic scalar function call: name ( arg, ... ) -- e.g. length(p),
                // zoned_datetime('2010-01-01'). Dispatched by name in the evaluator/typechecker.
                advance(); // consume function name
                consume(TokenType::LPAREN, "Expected '(' after function name");
                std::vector<std::unique_ptr<Expression>> args;
                if (!check(TokenType::RPAREN)) {
                    do {
                        args.push_back(parse_expression());
                    } while (match(TokenType::COMMA));
                }
                consume(TokenType::RPAREN, "Expected ')' after function arguments");
                std::string lname = var;
                std::transform(lname.begin(), lname.end(), lname.begin(),
                               [](unsigned char c) { return std::tolower(c); });

                // Only names the evaluator actually implements may parse. Anything else used to build a
                // FunctionCallExpr that typechecked as ANY and evaluated to NULL, so a query calling an
                // unimplemented function -- abs(), toInteger(), labels(), coalesce(), shortestPath() --
                // silently produced NULLs and plausible-looking wrong answers instead of failing. Reject
                // it here, where the name is known and the error can name it.
                if (!is_supported_scalar_function(lname)) {
                    throw std::runtime_error("Unknown function: " + var + "(). Supported scalar functions: " +
                                             supported_scalar_function_list());
                }
                return std::make_unique<FunctionCallExpr>(std::move(lname), std::move(args));
            }
        }

        advance();
        if (match(TokenType::DOT)) {
            std::string prop = peek().text;
            TokenType type = peek().type;
            if (type == TokenType::NAME || (type >= TokenType::TRUE_KW && type < TokenType::LPAREN)) {
                advance();
            } else {
                throw std::runtime_error("Expected property name after '.' (found: " + prop + ")");
            }
            return std::make_unique<PropertyLookupExpr>(var, prop);
        }
        return std::make_unique<VariableExpr>(var);
    }

    // A keyword that reaches expression position (all keyword-led forms -- literals, NOT, EXISTS --
    // were handled above) can only be an identifier shadowed by a reserved word: keep variables and
    // aliases named e.g. `with` referencable.
    {
        TokenType t = peek().type;
        if (t >= TokenType::TRUE_KW && t < TokenType::LPAREN) {
            std::string var = peek().text;
            advance();
            if (match(TokenType::DOT)) {
                std::string prop = peek().text;
                TokenType pt = peek().type;
                if (pt == TokenType::NAME || (pt >= TokenType::TRUE_KW && pt < TokenType::LPAREN)) {
                    advance();
                } else {
                    throw std::runtime_error("Expected property name after '.' (found: " + prop + ")");
                }
                return std::make_unique<PropertyLookupExpr>(var, prop);
            }
            return std::make_unique<VariableExpr>(var);
        }
    }

    throw std::runtime_error("Unexpected token in expression: " + peek().text);
}

/**
 * @brief Class helper method to tokenize and parse a GQL query string into its AST.
 * 
 * @param query The GQL query string to process.
 * @return GqlQuery The parsed AST query object.
 */
GqlQuery GqlParser::parse(const std::string& query) {
    auto tokens = GqlLexer::tokenize(query);
    GqlParser parser(tokens);
    return parser.parse_query();
}

std::shared_ptr<LabelExpression> GqlParser::parse_label_expression(bool allow_keyword_ops) {
    return parse_label_or(allow_keyword_ops);
}

std::shared_ptr<LabelExpression> GqlParser::parse_label_or(bool allow_keyword_ops) {
    auto left = parse_label_and(allow_keyword_ops);
    while (check(TokenType::PIPE) || (allow_keyword_ops && check(TokenType::OR))) {
        advance(); // consume '|' or 'OR'
        auto right = parse_label_and(allow_keyword_ops);
        auto node = std::make_shared<LabelExpression>();
        node->kind = LabelExprKind::OR;
        node->left = std::move(left);
        node->right = std::move(right);
        left = std::move(node);
    }
    return left;
}

std::shared_ptr<LabelExpression> GqlParser::parse_label_and(bool allow_keyword_ops) {
    auto left = parse_label_factor(allow_keyword_ops);
    while (check(TokenType::AMP) || (allow_keyword_ops && check(TokenType::AND))) {
        advance(); // consume '&' or 'AND'
        auto right = parse_label_factor(allow_keyword_ops);
        auto node = std::make_shared<LabelExpression>();
        node->kind = LabelExprKind::AND;
        node->left = std::move(left);
        node->right = std::move(right);
        left = std::move(node);
    }
    return left;
}

std::shared_ptr<LabelExpression> GqlParser::parse_label_factor(bool allow_keyword_ops) {
    if (match(TokenType::BANG) || (allow_keyword_ops && match(TokenType::NOT))) {
        auto expr = parse_label_factor(allow_keyword_ops);
        auto node = std::make_shared<LabelExpression>();
        node->kind = LabelExprKind::NOT;
        node->expr = std::move(expr);
        return node;
    }
    if (match(TokenType::LPAREN)) {
        // Parenthesised: a ')' delimits it, so the keyword spellings are unambiguous again inside.
        auto expr = parse_label_expression(true);
        consume(TokenType::RPAREN, "Expected ')' to close label expression");
        return expr;
    }
    if (match(TokenType::PERCENT)) {
        auto node = std::make_shared<LabelExpression>();
        node->kind = LabelExprKind::WILDCARD;
        return node;
    }
    if (check(TokenType::NAME)) {
        auto text = peek().text;
        advance();
        auto node = std::make_shared<LabelExpression>();
        node->kind = LabelExprKind::LITERAL;
        node->name = text;
        return node;
    }
    throw std::runtime_error("Expected label name or expression in node/edge pattern (found: " + peek().text + ")");
}

} // namespace ragedb::gql
