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
