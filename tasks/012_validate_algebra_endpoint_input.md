# Validate the schema algebra endpoint and round out its API

Priority: medium.

## Findings (src/main/handlers/Schema.cpp:590-618)

1. No whitelist validation of trait strings and no case normalization of the values:
   `GqlVirtualCatalog::set_relationship_algebraic_properties` normalizes only the TYPE key
   (GqlVirtualCatalog.h:141-151) while the optimizers look up exact lowercase literals
   ("symmetric", "transitive", ...). `POST ... /algebra` with `["Symmetric"]` or `["symetric"]`
   returns 200 but the trait never fires -- a silent no-op. Validate against the known traits
   (lowercase, reject unknowns with 400).
2. The relationship type's existence is never checked -- typo'd type names are accepted with 200
   (other type handlers do check).
3. Write-only, volatile API: no `GET .../algebra` to inspect and no `DELETE` (clearing requires
   knowing to POST `[]`), unlike the index endpoints' GET/POST/DELETE triples. The catalog is
   in-memory only, so traits vanish on restart with no restore path -- document or persist.
4. (Cache clearing on trait change is covered by task 006.)
