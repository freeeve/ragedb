# Bound and stop copying the semantic caches; yield in pair loops

Priority: high -- reactor stalls and O(V^2) memory/copies.

## Findings

1. Whole-graph computation inside per-row execution: `ReachablePeered` is a BFS from every node
   -- O(V*(V+E)) time, O(V^2) pairs, built with zero yields -- and runs synchronously on first
   cache miss (PathTraverser.cpp:1487; Connectivity.cpp:86-121). Even after the .get0() fix
   (task 002) this stalls the shard for the whole computation.
2. Full deep copies per call: `descendants = TransitiveReachabilityCache::local().get(rel_type)`
   copies an O(V^2) map-of-sets per invocation, and `traverse_match_statement` runs once per input
   row (PathTraverser.cpp:1485, 1492); the WCC map is likewise copied per row (1367, 1371). The
   miss path does set-then-copy-back (1491-1492). Return shared_ptr/const references from the
   caches instead.
3. Unbounded caches: TransitiveReachabilityCache.h:35 / WccCache.h:34 have no size limit or
   eviction, and are replicated thread_local per shard -- worst case shards x rel_types x O(V^2).
   Add a size budget/eviction policy, and document the footprint.
4. O(N^2) synchronous pair loops with no `seastar::maybe_yield`
   (PathTraverser.cpp:1414-1449, 1522-1556) materialize up to N^2 GqlRow copies in one reactor
   task when neither endpoint is bound.
5. All five semantic passes run per query even when no algebraic traits are registered -- add a
   cheap `if (catalog empty) return` guard. Also `normalize_name`
   (src/gql/GqlVirtualCatalog.h:141-151) allocates per lookup, up to 4 lookups per edge per pass;
   its comment also says "uppercase" while the code lowercases -- fix both.
