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

// The value-comparison primitives underneath ORDER BY, MIN/MAX and every WHERE comparison. compare_properties
// orders the scalar property variant; compare_gql_values wraps it and adds list/type ordering. Both are pure,
// so they are pinned directly rather than through a query result.

#include <catch2/catch.hpp>
#include <memory>
#include <string>
#include <vector>
#include "../../src/gql/GqlValue.h"

using namespace ragedb;
using namespace ragedb::gql;

namespace {
// A LIST GqlValue built from integer elements.
GqlValue int_list(std::vector<int64_t> xs) {
    GqlValue v;
    v.type = GqlValue::LIST;
    v.list = std::make_shared<std::vector<GqlValue>>();
    for (auto x : xs) v.list->push_back(GqlValue(property_type_t{x}));
    return v;
}
}  // namespace

TEST_CASE("compare_properties orders null first and compares within a type", "[gql_value]") {
    // The null (monostate) alternative sorts before any actual value.
    REQUIRE(compare_properties(property_type_t{}, property_type_t{(int64_t)5}) < 0);
    REQUIRE(compare_properties(property_type_t{(int64_t)5}, property_type_t{}) > 0);
    REQUIRE(compare_properties(property_type_t{}, property_type_t{}) == 0);
    // Within a single type: integers, floats, strings (lexicographic) and booleans (false < true).
    REQUIRE(compare_properties(property_type_t{(int64_t)3}, property_type_t{(int64_t)5}) < 0);
    REQUIRE(compare_properties(property_type_t{2.5}, property_type_t{1.5}) > 0);
    REQUIRE(compare_properties(property_type_t{std::string("apple")}, property_type_t{std::string("banana")}) < 0);
    REQUIRE(compare_properties(property_type_t{false}, property_type_t{true}) < 0);
}

TEST_CASE("compare_properties compares an integer and a float by numeric value", "[gql_value]") {
    REQUIRE(compare_properties(property_type_t{(int64_t)5}, property_type_t{5.0}) == 0);
    REQUIRE(compare_properties(property_type_t{(int64_t)5}, property_type_t{5.5}) < 0);
    REQUIRE(compare_properties(property_type_t{5.5}, property_type_t{(int64_t)5}) > 0);
    // A non-numeric cross-type pair has no defined order and compares equal.
    REQUIRE(compare_properties(property_type_t{std::string("x")}, property_type_t{(int64_t)5}) == 0);
}

TEST_CASE("compare_gql_values compares lists by length then element by element", "[gql_value]") {
    REQUIRE(compare_gql_values(int_list({1, 2}), int_list({1, 2, 3})) < 0);   // the shorter list sorts first
    REQUIRE(compare_gql_values(int_list({1, 2}), int_list({1, 3})) < 0);      // equal length, 2 < 3
    REQUIRE(compare_gql_values(int_list({1, 2}), int_list({1, 2})) == 0);
}

TEST_CASE("compare_gql_values orders values of different types consistently", "[gql_value]") {
    GqlValue prop(property_type_t{(int64_t)1});
    GqlValue list = int_list({1});
    int forward = compare_gql_values(prop, list);
    REQUIRE(forward != 0);                                 // values of different types are never equal
    REQUIRE(compare_gql_values(list, prop) == -forward);   // and the ordering is antisymmetric
}

namespace {
bool is_null_value(const GqlValue& v) {
    return v.type != GqlValue::PROPERTY || std::holds_alternative<std::monostate>(v.property);
}
GqlValue prop_int(int64_t x) { return GqlValue(property_type_t{x}); }
GqlValue prop_double(double x) { return GqlValue(property_type_t{x}); }
GqlValue prop_string(std::string x) { return GqlValue(property_type_t{x}); }
GqlValue prop_bool(bool x) { return GqlValue(property_type_t{x}); }
}  // namespace

TEST_CASE("apply_cast to STRING renders each primitive", "[gql_value]") {
    REQUIRE(std::get<std::string>(apply_cast(prop_int(42), CastType::STRING).property) == "42");
    REQUIRE(std::get<std::string>(apply_cast(prop_double(2.5), CastType::STRING).property) == "2.5");
    REQUIRE(std::get<std::string>(apply_cast(prop_bool(true), CastType::STRING).property) == "true");
}

TEST_CASE("apply_cast to INTEGER parses whole strings, rounds floats, and NULLs the rest", "[gql_value]") {
    REQUIRE(std::get<int64_t>(apply_cast(prop_string("42"), CastType::INTEGER).property) == 42);
    REQUIRE(is_null_value(apply_cast(prop_string("42x"), CastType::INTEGER)));   // a partial parse is NULL
    REQUIRE(is_null_value(apply_cast(prop_string("abc"), CastType::INTEGER)));
    REQUIRE(std::get<int64_t>(apply_cast(prop_double(2.7), CastType::INTEGER).property) == 3);   // rounds
    REQUIRE(std::get<int64_t>(apply_cast(prop_double(2.4), CastType::INTEGER).property) == 2);
    REQUIRE(std::get<int64_t>(apply_cast(prop_bool(true), CastType::INTEGER).property) == 1);
}

TEST_CASE("apply_cast to FLOAT widens integers and parses whole strings", "[gql_value]") {
    REQUIRE(std::get<double>(apply_cast(prop_int(5), CastType::FLOAT).property) == 5.0);
    REQUIRE(std::get<double>(apply_cast(prop_string("2.5"), CastType::FLOAT).property) == 2.5);
    REQUIRE(is_null_value(apply_cast(prop_string("2.5x"), CastType::FLOAT)));
}

TEST_CASE("apply_cast to BOOLEAN uses truthiness and case-insensitive keywords", "[gql_value]") {
    REQUIRE(std::get<bool>(apply_cast(prop_int(0), CastType::BOOLEAN).property) == false);
    REQUIRE(std::get<bool>(apply_cast(prop_int(5), CastType::BOOLEAN).property) == true);
    REQUIRE(std::get<bool>(apply_cast(prop_string("TRUE"), CastType::BOOLEAN).property) == true);
    REQUIRE(is_null_value(apply_cast(prop_string("yes"), CastType::BOOLEAN)));   // not a boolean keyword
}

TEST_CASE("apply_cast returns NULL for a null or non-primitive value", "[gql_value]") {
    REQUIRE(is_null_value(apply_cast(GqlValue(), CastType::INTEGER)));
    GqlValue list;
    list.type = GqlValue::LIST;
    list.list = std::make_shared<std::vector<GqlValue>>();
    REQUIRE(is_null_value(apply_cast(list, CastType::STRING)));
}
