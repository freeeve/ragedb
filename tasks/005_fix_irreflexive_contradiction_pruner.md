# Fix IrreflexiveContradictionPruner false-positive no_ops

Priority: critical -- entire queries silently return empty.

## Findings (src/gql/optimizer/IrreflexiveContradictionPruner.cpp)

1. Anonymous endpoints both have empty `variable` (GqlParser.cpp:995-1015) and
   `are_equal_vars("", "")` returns true via the `v1 == v2` shortcut (line 56), so
   `MATCH ()-[:parent_of]->() RETURN count(*)` on an irreflexive relation is marked no_op and
   returns empty (lines 116-119). Skip when either variable is empty.
2. No `is_variable_length` check (lines 110-115): irreflexivity forbids 1-hop self-loops, not
   cycles. `MATCH (a)-[:spouse_of*2..2]->(a)` is satisfiable (a->b->a) but gets no_op'd.
3. Equalities are harvested from node/edge `where_expr` of ALL matches including `is_optional`
   ones (lines 93-104), then used to no_op the whole query. An equality inside an OPTIONAL MATCH
   only filters the optional binding.
4. `x.id = y.id` is treated as node identity (line 46), but "id" can be an ordinary user property
   (AlglibPerformanceTests.cpp:31 declares one) -- two distinct nodes can share it. Same
   convention exists in DirectionSwapOptimizer; centralize and document, or restrict to genuine
   internal-id expressions.

## Acceptance

- Negative-control tests: anonymous endpoints, variable-length self-loops, OPTIONAL MATCH
  equalities, and `WHERE a = b OR ...` disjunctions must NOT no_op.
