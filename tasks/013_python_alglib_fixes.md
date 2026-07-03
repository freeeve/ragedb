# Fix pyragedb alglib model scoping, domain param, and silent failures

Priority: medium.

## Findings (python/pyragedb/semantics/std/alglib.py, model.py)

1. Model scoping: `_register_property` posts to `get_active_model()` (the last-constructed Model,
   model.py:23-24) rather than the model the relationship belongs to, even though `alglib` is
   exposed as an instance property. With two models, `m1.alglib.symmetric(rel)` silently POSTs to
   m2's host/graph. Resolve the model from self/the relationship.
2. `reflexive(rel, domain)` and `equivalence_relation(rel, domain)` (alglib.py:31-44) accept a
   `domain` argument and ignore it entirely -- not validated, not stored, not sent to the server;
   the C++ side treats reflexivity as globally true. Either implement domain end to end (see task
   004) or reject the parameter loudly.
3. Silent sync failure: the `requests.post` (alglib.py:17-23) has no timeout and swallows all
   RequestExceptions with bare `pass` -- traits get recorded client-side while the server never
   sees them, so queries silently run unoptimized. Add a timeout, log a warning (or raise), and
   share the fix with model.py's no-timeout pattern.
4. Tests (test/pyrage/test_alglib.py) only assert client-side set membership. Mock
   `requests.post` to verify URL, payload, and type casing; cover the TypeError branch for
   non-Relationship input and the cumulative-payload behavior; avoid writing schema into a live
   localhost server as a side effect.
