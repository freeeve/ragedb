# Remove or dedupe SymmetricTraversalOptimizer; fix path_variable reversal

Priority: medium.

## Findings

1. SymmetricTraversalOptimizer (src/gql/optimizer/SymmetricTraversalOptimizer.cpp:32-47) is a
   near-verbatim copy of DirectionSwapOptimizer (phase 21, which runs immediately before it and
   already reverses any match by the same criterion), but with a REGRESSED selectivity estimator:
   it returns INDEXED for any variable found in q_vars even with `intervals.empty()`
   (DirectionSwapOptimizer.cpp:52-54 correctly returns SCAN), so it reverses patterns on zero
   evidence. Since flipping direction while reversing node order is semantics-preserving for ANY
   relation, the symmetric gate buys nothing. Delete the pass, or reduce it and phase 21 to
   shared helpers (`reverse_match_pattern` and the estimator are currently duplicated).
2. `reverse_match_pattern` (SymmetricTraversalOptimizer.cpp:49-59, 96-98) reverses matches
   without guarding `path_variable`/`shortest_path_kind`, so `MATCH p = (a)-[:knows]->(b)`
   returns a path with reversed node order. The same latent bug exists in the pre-existing
   DirectionSwapOptimizer -- fix both in the shared helper.
