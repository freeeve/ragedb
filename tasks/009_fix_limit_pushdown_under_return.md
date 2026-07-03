# Fix LIMIT pushdown returning fewer rows than exist

Priority: high -- correctness: `LIMIT n` can return < n rows when more matches exist.

The pushed-down LIMIT is used as the physical scan limit, but several filters are applied AFTER
the scan, so scanning exactly `limit` rows under-returns.

## Findings

1. Residual WHERE: limit pushdown (src/gql/GqlExecutor.cpp:329-332) only checks for ORDER BY and
   aggregates. Predicate pushdown can leave a residual `query.where_expr`
   (GqlOptimizer.cpp:692) evaluated per-row after traversal (GqlExecutor.cpp:617). A query like
   `MATCH (n:Person) WHERE <non-pushable predicate> RETURN n LIMIT 10` scans 10 nodes, filters,
   and returns fewer than 10. Disable pushdown when a residual where_expr (query-level or
   node/edge-level) survives, or over-scan and re-drive.
2. Multi-label patterns: `(n:A|B)` scans via `AllNodesPeered` without a label filter and
   post-filters at PathTraverser.cpp:939 -- same under-return with a pushed limit. Same for
   node-level residual `where_expr` applied at PathTraverser.cpp:948.
3. Relationship-index path: `traverse_from_relationship_index` (PathTraverser.cpp:707, 743-756)
   fetches `scan_limit` relationships then post-filters by label expression and remaining
   properties/filters -- under-return when a LIMIT is pushed.

## Acceptance

- Pushdown only when every remaining predicate is guaranteed applied inside the scan; otherwise
  scan with the default limit and truncate after filtering.
- Tests: LIMIT with residual WHERE, LIMIT with multi-label pattern, LIMIT via the
  relationship-index path, each on data where post-filters reject some scanned rows.
