# wopibaga gaps

## Closed

### W1 — ~~no X-WOPI-Proof~~ — closed 2026-10-31

`wopi_verify_proof(proof_b64, msg, mod_b64, exp_b64)`: RSA PKCS#1 v1.5
проверка с SHA-256 (чрез `std/crypto/rsa.baga` + `sha256_b`). Каноничното
съобщение: `wopi_proof_message(timestamp, token, method, path, body_hash)`.
Публичният ключ (mod/exp base64) идва от discovery `<proof-key>`.

## Closed

### W2 — PutRelative / RenameFile / Delete — closed 2026-10-31

`wopi_apply_ext` поддържа `PUT_RELATIVE` (с `X-WOPI-SuggestedTarget`,
URL-decode + валидация на име), `RENAME_FILE` (с `X-WOPI-RequestedName`) и
`DELETE`. Lock-проверката е същата като при PUT: 409 при чужд lock.
`wopi_valid_name` отхвърля `/ \ : * ? " < > |`, контролни знаци, празно
и само точки. `wopi_suggested_name` взема последния сегмент след `/`
след URL-decode.
