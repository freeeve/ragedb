## Cross-pollinate learnings with the sibling GQL engines

ragedb, `gochickpeas` (Go), and `rustychickpeas` (Rust) are independent Cypher/GQL engines over the same
LDBC workloads. When you hit a **surprising, non-obvious learning** -- a subtle correctness bug class, a
planner/executor technique with a real win, a build/infra gotcha, a spec/conformance finding -- file it to
BOTH siblings for their review, since it may cross-pollinate. They already do this to us (their task
histories are full of cross-project asks); reciprocate.

- How: `taskman file gochickpeas "<learning>"` and `taskman file rustychickpeas "<learning>"`. Frame it as a
  "when you hit this / check your engine" note, not a bug report against them. Include the shape, the symptom
  with numbers if measured, and the fix; cite the ragedb commit/task.
- What qualifies: surprises worth another engine's time -- e.g. "an anonymous destination property-map was
  pruned before its own filter ran -> silent 0 rows", "COUNT{} degree-rewrite ignores the far-node label",
  "GNU ld OOMs the tests link at 16GB; lld links it in a fraction of the memory". NOT routine fixes.
- Skip it when the learning is ragedb-specific plumbing with no transfer value. When in doubt, file it --
  a cheap cross-project note beats a missed idea. Do this at the moment of the insight, same as committing.

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
