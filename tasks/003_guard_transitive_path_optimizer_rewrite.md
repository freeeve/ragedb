# Guard TransitivePathOptimizer rewrite against unsound cases

Priority: critical -- silently wrong results whenever a relation is annotated transitive.

## Findings (src/gql/optimizer/TransitivePathOptimizer.cpp + PathTraverser.cpp Case 0.6)

1. Edge direction ignored (TransitivePathOptimizer.cpp:55-68; PathTraverser.cpp:1474-1555):
   the executor always tests `descendants[nodes[0]] contains nodes[1]` with directed=RIGHT
   semantics. `(a)<-[:T*]-(b)` returns reversed results; undirected `-[:T*]-` loses
   reverse-direction reachability.
2. Quantifier bounds discarded (lines 63-67): `*2..2` and `*1..2` become unbounded reachability
   (extra rows); `*0..` loses mandatory 0-hop a==b rows (missing rows). Transitivity does not make
   hop bounds unobservable because the stored graph need not materialize closure edges.
3. Edge predicates and bindings dropped: `edge.properties`, `property_filters`, `where_expr`,
   the edge variable (should bind a RELATIONSHIP_LIST), `path_variable` (fabricated 2-node Path
   with zero relationships breaks `length(p)`/`relationships(p)`), and `shortest_path_kind`
   (Case 0.6 at PathTraverser.cpp:1474 precedes the shortest-path branch at :1602, hijacking
   ANY SHORTEST).
4. Cyclic self-pairs: `ReachablePeered` never emits `(s,s)` (start pre-inserted into visited,
   Connectivity.cpp:99-121), so on a cyclic relation `(a)-[:T*]->(b)` loses b==a rows the
   original traversal finds.
5. Shortcut-edge pruning (lines 104-136) decides all prunes against the ORIGINAL adjacency and
   removes them simultaneously -- in cycles two edges can mutually justify each other and both
   get pruned, leaving variables unbound. Pruned matches also drop labels, properties, filters,
   and edge variables wholesale (lines 132-135), and pruning changes row multiplicity with
   parallel edges (no count compensation).

## Acceptance

- Rewrite fires only for: direction RIGHT (or executor handles LEFT/ANY correctly), unbounded
  `*`/`*1..` with no edge variable, no edge predicates, no path variable, no shortest selector.
- Shortcut pruning is iterative (rebuild adjacency after each prune, or exclude pruned edges from
  justification paths) and refuses to prune matches carrying any constraint or referenced variable.
- Semantic-equivalence tests vs NO_SEMANTIC execution for every guarded case, including cycles.
