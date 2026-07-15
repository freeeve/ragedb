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

#ifndef RAGEDB_TRANSITIVEREACHABILITYCACHE_H
#define RAGEDB_TRANSITIVEREACHABILITYCACHE_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <memory>
#include <cstdint>

namespace ragedb::gql {

/**
 * Thread-local cache for Transitive Reachability descendant sets.
 * Keyed by relationship type. Avoids repeated calculations of transitive closures.
 *
 * Footprint: one entry per distinct relationship type queried, per shard. Each entry is a
 * shared snapshot of the closure -- up to O(V^2) for a densely reachable graph. Real schemas
 * have a handful of relationship types, so the natural bound is small; kMaxEntries is only a
 * runaway guard against a pathological stream of distinct type strings. Entries are re-derived
 * on a miss, so dropping them on overflow costs a recompute, never correctness.
 */
class TransitiveReachabilityCache {
public:
    using DescendantsMap = std::map<uint64_t, std::unordered_set<uint64_t>>;

private:
    // Maps relationship type -> descendants map (node_id -> set of reachable descendant node_ids).
    // Entries are immutable shared snapshots: readers hold a shared_ptr instead of copying the
    // (potentially O(V^2)) map per query row, and invalidation just drops the cache's reference.
    std::unordered_map<std::string, std::shared_ptr<const DescendantsMap>> cache;
    static constexpr size_t kMaxEntries = 64;

public:
    static TransitiveReachabilityCache& local() {
        thread_local TransitiveReachabilityCache instance;
        return instance;
    }

    void clear() {
        cache.clear();
    }

    void invalidate(const std::string& rel_type) {
        cache.erase(rel_type);
    }

    bool has(const std::string& rel_type) const {
        return cache.find(rel_type) != cache.end();
    }

    // Returns the cached snapshot, or nullptr if absent.
    std::shared_ptr<const DescendantsMap> get(const std::string& rel_type) const {
        auto it = cache.find(rel_type);
        return it != cache.end() ? it->second : nullptr;
    }

    std::shared_ptr<const DescendantsMap> set(const std::string& rel_type, DescendantsMap&& descendants) {
        if (cache.size() >= kMaxEntries && cache.find(rel_type) == cache.end()) {
            cache.clear();
        }
        auto snapshot = std::make_shared<const DescendantsMap>(std::move(descendants));
        cache[rel_type] = snapshot;
        return snapshot;
    }
};

} // namespace ragedb::gql

#endif // RAGEDB_TRANSITIVEREACHABILITYCACHE_H
