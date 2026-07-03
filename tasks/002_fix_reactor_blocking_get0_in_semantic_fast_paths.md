# Fix reactor-blocking .get0() in equivalence/transitive fast paths (crash on smp>1)

Priority: critical -- process abort on first production use with multiple cores.

## Findings

- PathTraverser.cpp:1369 (Case 0.5, equivalence) and :1487 (Case 0.6, transitive reachability)
  synchronously call `WeaklyConnectedComponentsPeered`/`ReachablePeered`, which internally call
  `.get0()` on cross-shard futures (src/graph/algorithms/Connectivity.cpp:32-34, 86-87,
  126-127). Those algorithms were written for the Lua path, which runs inside `seastar::async`
  (src/graph/Shard.cpp:708). The GQL HTTP handler (src/main/handlers/Gql.cpp:52) runs
  `GqlExecutor::execute` as a plain future chain on the reactor -- `.get0()` on an unready
  future triggers seastar's "attempted to wait from reactor thread" assertion and aborts.
- Tests pass only because the Catch2 harness runs inside a seastar thread context
  (test/gql/GqlExecutorReadTests.cpp calls `.get()` at top level).

## Acceptance

- Fast paths consume futures asynchronously (`.then()` chains) or the whole GQL execution is
  moved into a `seastar::thread` deliberately (document the choice).
- A multi-shard (smp >= 2) test exercises a transitive-rewritten query over the HTTP/future path.
