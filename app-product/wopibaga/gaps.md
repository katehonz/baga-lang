# wopibaga gaps

## Open

### W1 — no X-WOPI-Proof

The office server's RSA proof is not checked. The access token is the
secret. A leaked token can be used until it expires.

### W2 — PutRelative and rename

`PUT_RELATIVE`, rename, and delete are answered 501 by `wopi_apply`.
