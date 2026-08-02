# ADR 0002: Explicit tagged TValue

## Status

Accepted (v0.1)

## Decision

Use explicit `{payload, type, aux}` tagged values. Defer NaN-boxing until after GC and interpreter stability.

## Consequences

Clearer sanitizer/debug story; slightly higher memory traffic than NaN-boxing.
