# Make the 100k GQL scan limit explicit, configurable, and observable

Priority: high -- silently incomplete results and wrong aggregates on graphs > 100k nodes.

## Findings

1. `size_t scan_limit = (limit > 0) ? limit : 100000;` is a duplicated magic number
   (src/gql/executor/PathTraverser.cpp:52 and :707). For any multi-hop pattern the start-node
   scan always uses the 100k cap (`start_node_limit = prep_pattern.edges.empty() ? limit : 0`
   at PathTraverser.cpp:928), so on graphs with more than 100k candidate start nodes results --
   including COUNT and other aggregates -- are silently truncated. No warning, no error, no
   config knob.
2. Multi-filter intersection over truncated lists (PathTraverser.cpp:72-104): with multiple
   property filters, each `FindNodesPeered` list is independently capped at scan_limit BEFORE the
   leapfrog intersection, so the intersection can miss valid nodes whenever any single filter
   matches more than the cap -- wrong results even when the final result set is tiny.

## Acceptance

- Named constant (e.g. DEFAULT_SCAN_LIMIT) in one place; server-configurable (config option
  and/or per-query escape hatch).
- Queries that hit the cap surface it: at minimum a warning in the response/summary, ideally an
  error mode for aggregate queries where truncation means a wrong answer.
- The multi-filter path either intersects unbounded id streams or picks the most selective filter
  to drive and post-filters the rest, instead of intersecting independently truncated lists.

## Resolution

Rather than making the cap configurable/observable, the cap was removed: with no explicit query
LIMIT, both the node start-scan (`get_start_nodes`) and the relationship-index anchor scan
(`traverse_from_relationship_index`) now derive their scan bound from the actual candidate count
instead of the hardcoded 100000.

- Node scan: `AllNodesCountPeered()` / `AllNodesCountPeered(label)`.
- Relationship-index scan: `FindRelationshipCountPeered(label, prop, EQ, val)` (the anchor op is
  always EQ here, so the count matches the scan exactly).

Using the real count keeps the underlying `reserve()` / `max = skip + limit` accumulation bounded
(a giant sentinel like SIZE_MAX would over-reserve / overflow), so nothing is silently truncated.
This also fixes finding #2 for free: every per-filter `FindNodesPeered` list is now bounded by the
label's total node count, so no filter list is truncated before the leapfrog intersection.

Trade-off: an unbounded query now costs one extra count round-trip, and materializes the full
result set in memory (the intended behavior when no LIMIT is given). A >100k-node regression test is
a good follow-up but needs a large fixture / build environment to exercise.

## Follow-up (deploy-surfaced): count without materialization

Deploying the above to a real SF1 graph (Comment ~1.7M nodes) surfaced that removing the cap made
`count(Comment)` materialize every Node just to size the aggregate group -> std::bad_alloc. Fixed by
a guarded fast path in execute_query_internal: `MATCH (n:Label) RETURN count(n)` (single node, no
edges, optional single filter, no WHERE/ORDER/LIMIT/DISTINCT) is answered from the shard count
indexes (AllNodesCountPeered / FindNodeCountPeered) with no Node materialization; anything more
complex falls back. Verified live: count(Comment)=1,739,438 and count(n)=2,888,570 in <1ms (were OOM).

Remaining edges (candidates for a future task): non-aggregate unbounded scans (`MATCH (c:Comment)
RETURN c` with no LIMIT) still materialize all rows; multi-filter counts (>1 filter) fall back to the
materializing path.
