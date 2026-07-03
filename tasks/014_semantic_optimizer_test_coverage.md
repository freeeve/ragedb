# Add semantic-equivalence and invalidation test coverage for phases 22-26

Priority: high -- the current tests could not have caught any of the bugs in tasks 001-006.

## Gaps

1. No semantic-equivalence tests: nothing compares fast-path output against NO_SEMANTIC/
   unoptimized traversal on the same data. Needed for: LEFT/undirected patterns, bounded
   quantifiers (`*1..2`, `*0..`, `*2..2`), edge property filters, edge-variable binding,
   path-variable contents, SHORTEST selectors, TRAIL-mode multiplicity/COUNT, cycles (a==b rows).
2. No negative controls: trait absent -> no rewrite; non-literal/multi-label edge label -> no
   rewrite; anonymous endpoints and OPTIONAL MATCH equalities must NOT no_op (task 005); bounded
   ranges must NOT be coalesced.
3. TransitiveReachabilityCache appears nowhere under test/ -- Case 0.6 results, population, and
   invalidation are entirely untested.
4. The WccCache invalidation test (test/gql/GqlExecutorReadTests.cpp:310-359) covers only a
   same-shard relationship ADD. Missing: deletes, node-delete cascade, multi-shard invalidation,
   REST handler paths, plan-cache staleness across a trait update (task 006).
5. The Phase 22 optimizer test (test/gql/optimizer/AlglibOptimizerTests.cpp:25-40) does not
   isolate phase 22 -- `WHERE b.id = 1` makes phase 21 alone produce the same outcome; the test
   passes with the new pass deleted.
6. The "Phase 23" benchmark (test/gql/optimizer_performance/AlglibPerformanceTests.cpp:83-89)
   uses only single-hop patterns, so the reachability fast path it is nominally about never runs.
7. A multi-shard (smp >= 2) HTTP-path test for the fast paths (task 002).
8. LIMIT pushdown under-return cases (task 009) and 100k-truncation surfacing (task 008).
