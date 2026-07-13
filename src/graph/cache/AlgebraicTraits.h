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

#ifndef RAGEDB_ALGEBRAICTRAITS_H
#define RAGEDB_ALGEBRAICTRAITS_H

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ragedb {

/**
 * Thread-local registry of algebraic traits (symmetric, transitive, ...) per relationship type,
 * keyed by lowercased type name. Lives in the graph layer so the write paths that must
 * invalidate the semantic caches can consult it without depending on the GQL layer; the GQL
 * virtual catalog delegates its trait accessors here.
 */
class AlgebraicTraits {
private:
    std::unordered_map<std::string, std::unordered_set<std::string>> traits;

public:
    static AlgebraicTraits& local() {
        thread_local AlgebraicTraits instance;
        return instance;
    }

    // Lowercases a relationship type name into the registry's canonical key form.
    static std::string normalize(std::string name) {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        return name;
    }

    void set(const std::string& rel_type, const std::unordered_set<std::string>& props) {
        traits[normalize(rel_type)] = props;
    }

    bool has(const std::string& rel_type, const std::string& prop) const {
        auto it = traits.find(normalize(rel_type));
        if (it != traits.end()) {
            return it->second.count(prop) > 0;
        }
        return false;
    }

    // Whether the type carries any trait at all: the gate for skipping cache-invalidation
    // broadcasts on write paths (no traits means no fast path could have cached anything).
    bool any(const std::string& rel_type) const {
        auto it = traits.find(normalize(rel_type));
        return it != traits.end() && !it->second.empty();
    }

    const std::unordered_map<std::string, std::unordered_set<std::string>>& all() const {
        return traits;
    }

    void clear() {
        traits.clear();
    }
};

} // namespace ragedb

#endif // RAGEDB_ALGEBRAICTRAITS_H
