# wopibaga gaps

## Open

### W1 — no X-WOPI-Proof

The office server's RSA proof is not checked. The access token is the
secret. A leaked token can be used until it expires.

## Closed

### W2 — PutRelative / RenameFile / Delete — closed 2026-10-31

`wopi_apply_ext` поддържа `PUT_RELATIVE` (с `X-WOPI-SuggestedTarget`,
URL-decode + валидация на име), `RENAME_FILE` (с `X-WOPI-RequestedName`) и
`DELETE`. Lock-проверката е същата като при PUT: 409 при чужд lock.
`wopi_valid_name` отхвърля `/ \ : * ? " < > |`, контролни знаци, празно
и само точки. `wopi_suggested_name` взема последния сегмент след `/`
след URL-decode.
