# Fix AntisymmetricLoopCollapser dropping constraints (affects ALL queries)

Priority: critical -- silently wrong results, no algebraic traits required to trigger.

## Findings

1. `prune_redundant_single_node_matches` (src/gql/optimizer/AntisymmetricLoopCollapser.cpp:92-119) checks
   `properties`, `property_filters`, and `where_expr` but NOT `label_expr`, and it is called
   unconditionally at the end of the pass (line 224) for every SINGLE query -- even when no
   collapse occurred. `MATCH (x:Person) MATCH (x)-[:KNOWS]->(y) RETURN x,y` silently loses the
   `:Person` constraint.
2. `merge_pattern_nodes` (lines 30-47) merges properties/filters/where but drops `src.label_expr`.
   `MATCH (a:A)-[:part_of]->(b:B)-[:part_of]->(a)` loses the `:B` constraint after collapse.
3. `delete_edge_at` (lines 69-90) hoists only `where_expr`; the deleted edge's inline `properties`
   and `property_filters` are dropped, and there is no check that the deleted edge's `variable` is
   referenced in RETURN/ORDER BY/WHERE -- `RETURN r2` on a collapsed edge leaves `r2` unbound.
   A hoisted edge `where_expr` referencing its own edge variable becomes dangling.
4. `match_mode`/`path_mode` are not consulted (lines 131-224). Under default TRAIL/DIFFERENT_EDGES,
   `(a)-[:R]->(b)-[:R]->(a)` requires two distinct edges; after collapse one stored self-loop
   suffices -- result set and counts change.
5. Variable renaming misses `MatchStatement` scalar fields (`path_count_target_var`, `search_var`,
   `yield_var`, `yield_score_var`, `path_variable`), and `rewrite_expr_vars`
   (src/gql/optimizer/OptimizerUtils.h:66-108, shared utility) does not traverse `SIZE_OP` and
   `IS_NULL_CHECK` expression kinds -- fix in the shared utility since other passes use it too.
6. Pass ordering: the irreflexive pruner (phase 24) runs before the collapser (phase 25) in
   GqlOptimizer.cpp:492-497, so a 2-cycle on an antisymmetric AND irreflexive relation (a logical
   contradiction) is collapsed to a self-loop and executed instead of being no_op'd.

## Acceptance

- Prune/merge/delete refuse to drop any label, property, filter, or referenced variable.
- Tests: label preservation through prune and collapse, RETURN of a collapsed edge variable,
  TRAIL-mode count equivalence vs unoptimized execution.
- Consider gating phases 22-26 behind a config flag until 001-005 land.
