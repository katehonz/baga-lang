# otelbaga — gaps

## O1 — OTLP export is fire-and-forget

File append + optional HTTP POST. No retry queue, no batching agent.

## O2 — no tracestate / baggage

## O3 — hex→base64 only for even-length hex

Odd lengths truncate the last nibble.
