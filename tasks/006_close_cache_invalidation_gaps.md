# Close WccCache/TransitiveReachabilityCache/GqlQueryCache invalidation gaps

Priority: critical -- stale caches return wrong query results after mutations.

## Findings

1. Node deletion never invalidates: `NodeRemovePeered` cascades relationship deletion
   (src/graph/peered/Node.cpp:139-210) with no WccCache/TransitiveReachabilityCache invalidation,
   and the REST node-delete handler clears neither. Delete a node via REST or Lua, then
   `MATCH (a)-[:T*]->(b)` still returns paths through the deleted node.
2. GqlExecutor write path (src/gql/GqlExecutor.cpp:1121-1123) clears only WccCache;
   TransitiveReachabilityCache is never cleared there.
3. The `clear_cache` admin command (GqlExecutor.cpp:1100) clears GqlQueryCache + WccCache but not
   TransitiveReachabilityCache -- no manual purge escape hatch for the most dangerous cache.
4. Bulk load (src/graph/peered/LoadCSV.cpp:303) adds relationships via `RelationshipAddToIncoming`
   with no invalidation.
5. The new HTTP endpoint `POST /db/{g}/schema/relationships/{type}/algebra`
   (src/main/handlers/Schema.cpp:611-618) updates GqlVirtualCatalog but never clears GqlQueryCache,
   whose entries are stored AFTER optimization (GqlExecutor.cpp:1219-1237) -- optimizer-set flags
   (`equivalence_partition_lookup`/`transitive_reachability_lookup`, GqlAst.h:308-309) are baked
   into cached plans. Removing traits leaves stale rewritten plans; adding traits never upgrades
   cached plans. GQL DDL already follows the right convention (invoke_on_all + clear); do the same
   here, and also invalidate WccCache/TransitiveReachabilityCache for that type.

## Acceptance

- Every topology mutation path (GQL write, REST relationship add/delete, REST node delete, Lua,
  LoadCSV) invalidates both semantic caches; trait changes clear the plan cache.
- Tests: invalidation after relationship delete, node-delete cascade, multi-shard invalidation,
  REST handler paths, and plan-cache staleness across a trait update.
