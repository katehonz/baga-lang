# mdtbaga — gaps

## M1 — no binary-safe `-bin` codec

Values are `str`. Callers base64-encode `-bin` keys at the edge (Go does
this in the transport).

## M2 — no from/to httpdbaga Request helper in this package

`mdt_from_http_pairs(ks, vs)` is the pure core; pbbaga / fmr wrap Request.
