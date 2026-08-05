# ctxbaga — gaps

## C1 — no cancel tree

`ctx_cancel` only flips this handle. Children are value copies; canceling
a parent does not auto-cancel descendants (Go's `WithCancel` tree is M2).

## C2 — no select integration

`ctx_done` is polled; no channel closed on cancel (needs CSP + language).
