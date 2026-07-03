# Guard EquivalenceClassOptimizer rewrite and define its semantics

Priority: critical -- silently wrong results whenever a relation is annotated as an equivalence.

## Findings (src/gql/optimizer/EquivalenceClassOptimizer.cpp + PathTraverser.cpp Case 0.5)

1. Same missing guards as the transitive rewrite (EquivalenceClassOptimizer.cpp:29-43):
   `min_hops`/`max_hops` discarded (`*2..3` returns the whole partition), edge
   properties/filters/variable ignored (Case 0.5 at PathTraverser.cpp:1356-1471 never reads
   them), `shortest_path_kind` not checked (Case 0.5 intercepts before the shortest-path branch),
   `path_variable` bound to a fabricated relationship-less Path (PathTraverser.cpp:1451).
2. Whole-graph reflexivity: WCC seeds every node in the graph as its own component
   (Connectivity.cpp:126-131), so `MATCH (a)-[:same_group*]->(b) RETURN a,b` returns `(n,n)` for
   every node of every label, including nodes with no `same_group` edges. The Python API's
   `domain` argument for `reflexive`/`equivalence_relation` is dropped on the floor
   (python/pyragedb/semantics/std/alglib.py:31-44) -- domain restriction is unimplemented end
   to end. Decide the intended semantics and implement or reject `domain`.
3. Multiplicity change: normal traversal (default PathMode::TRAIL) returns one row per distinct
   trail (multiple rows per (s,e) pair); the fast path returns exactly one row per pair
   (PathTraverser.cpp:1428-1433), changing COUNT(*) and duplicate semantics.
4. `stmt.is_optional` handling in both fast paths (PathTraverser.cpp:1452-1462, 1558-1568) is
   dead code -- both optimizers skip optional matches.

## Related design task

The whole trait design assumes "virtualized closure" semantics (annotated relations behave as
their logical closure regardless of stored edges) while un-annotated execution uses stored-edge
semantics. This dual semantics is nowhere documented or asserted; every soundness argument for
phases 23/25/26 depends on it. Document it in the optimizer headers and SEMANTIC_LAYER.md, and
make tests assert it explicitly.
