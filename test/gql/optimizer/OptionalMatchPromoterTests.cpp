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

// Promoting OPTIONAL MATCH to a required one discards every row whose optional side matched nothing, so
// it is sound only when the WHERE cannot be true for an unbound variable. The subtlety is that GQL's
// operators disagree about what an unbound variable produces: a comparison yields NULL, while IS [NOT]
// NULL yields a definite boolean. Negation therefore behaves differently over the two, and getting it
// wrong drops rows the query explicitly asked for -- silently, since the result still looks well-formed.

#include <catch2/catch.hpp>
#include <string>
#include "../../../src/gql/GqlParser.h"
#include "../../../src/gql/GqlOptimizer.h"
#include "../../../src/gql/GqlVirtualCatalog.h"

using namespace ragedb;
using namespace ragedb::gql;

namespace {
bool promoted(const std::string& text) {
    GqlVirtualCatalog::local().clear();
    auto query = GqlParser::parse(text);
    GqlOptimizer::optimize(query);
    REQUIRE_FALSE(query.matches.empty());
    return !query.matches[0].is_optional;
}

const std::string kOptional = "OPTIONAL MATCH (p:Person)-[:FRIEND]->(f:Person) WHERE ";
}  // namespace

TEST_CASE("optional promotion requires a predicate no unbound row can satisfy", "[gql_optimizer]") {
    SECTION("a comparison is null for an unbound variable, so those rows go anyway") {
        REQUIRE(promoted(kOptional + "f.age > 21 RETURN f"));
    }

    SECTION("IS NOT NULL is false for an unbound variable") {
        REQUIRE(promoted(kOptional + "f.age IS NOT NULL RETURN f"));
    }

    SECTION("negating a comparison keeps it null, so promotion is still sound") {
        // NOT NULL is NULL, never true -- the row is filtered either way.
        REQUIRE(promoted(kOptional + "NOT (f.age > 21) RETURN f"));
    }

    SECTION("one null-rejecting conjunct is enough") {
        REQUIRE(promoted(kOptional + "f.age > 21 AND p.name = 'x' RETURN f"));
    }
}

TEST_CASE("optional promotion is refused when an unbound row satisfies the predicate", "[gql_optimizer]") {
    SECTION("IS NULL is exactly what an unbound row satisfies") {
        REQUIRE_FALSE(promoted(kOptional + "f.age IS NULL RETURN f"));
    }

    SECTION("negating IS NOT NULL asks for the same rows the long way round") {
        // IS NOT NULL answers false for an unbound f, so NOT makes it TRUE and the row belongs in the
        // result. Recursing through the negation had promoted this and dropped those rows.
        REQUIRE_FALSE(promoted(kOptional + "NOT (f.age IS NOT NULL) RETURN f"));
    }

    SECTION("a conjunct on another variable does not rescue it") {
        REQUIRE_FALSE(promoted(kOptional + "NOT (f.age IS NOT NULL) AND p.age > 1 RETURN f"));
    }

    SECTION("a disjunction needs every branch to reject nulls") {
        REQUIRE_FALSE(promoted(kOptional + "f.age > 21 OR p.name = 'x' RETURN f"));
    }
}
