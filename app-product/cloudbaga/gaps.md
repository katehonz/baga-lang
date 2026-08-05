# cloudbaga — gaps

## G1 — serial connections

Accept handles one request per connection then closes (no keep-alive /
no go_bg). Fine for the probe; production should use httpdbaga threads
mode + a drain gate on SIGTERM.

## G2 — readiness is process-local only

No dependency checks (Postgres, etc.). `/readyz` is "not shutting down".
