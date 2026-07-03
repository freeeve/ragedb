# Await cache-invalidation broadcasts and consolidate the two invalidation layers

Priority: high -- read-your-writes violation and swallowed errors.

## Findings

1. Fire-and-forget invalidation: `(void)container().invoke_on_all(...)` at
   src/graph/peered/Relationship.cpp:213-218 (and sibling sites at 323, 417, 475, 569, 628, 664)
   is not awaited, so the write's HTTP 201/204 resolves before remote shards drop their caches --
   a client that POSTs a relationship and immediately queries can read stale reachability/WCC.
   Exceptions in the discarded future are silently dropped (seastar "ignored exceptional future").
   Existing convention (src/graph/peered/Types.cpp:72-162) awaits the broadcast; chain it before
   resolving the result.
2. Redundant second layer: handler-level `WccCache::local().clear()` at
   src/main/handlers/Relationships.cpp:157-159, 183-185, 217-219, 239-241, 269-271 duplicates the
   peered-layer per-type invalidation, wipes ALL relationship types on every single write
   (defeating the cache across types), omits TransitiveReachabilityCache, and is also
   fire-and-forget. Delete these five blocks.
3. Quality cleanup in Relationship.cpp: the `auto fut = [..]() mutable {...}; return fut().then`
   wrapper copies every parameter an extra time and leaves the body mis-indented with a stray
   `};`; the identical 8-line invalidation continuation is duplicated seven times -- extract a
   `seastar::future<> InvalidateAlgebraicCaches(std::string rel_type)` helper. The `if (removed)`
   guard at lines 661-663 is dead (`RelationshipRemoveIncoming` always returns true) --
   invalidate unconditionally after the removal chain.
4. Layering inversion: graph-core src/graph/peered/Relationship.cpp now includes
   src/gql/executor/* headers. Move the caches (or an invalidation interface) to a layer the
   graph core can legitimately depend on.
5. Performance: every relationship add/remove broadcasts invoke_on_all to every shard even when
   no algebraic annotations exist and both caches are empty -- bulk inserts pay
   N x shard-count messaging. Skip the broadcast when the catalog has no traits for the type
   (or when caches are empty).
