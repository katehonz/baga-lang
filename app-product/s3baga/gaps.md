# s3baga gaps

## Open

### A1 — std HTTP client bodies are `str`

`http_request` cannot carry a null byte. This package writes the request
as `bytes` over TCP/TLS instead.

### A2 — path-style only

`S3_ENDPOINT/bucket/key`. No virtual-hosted `bucket.s3.amazonaws.com`.

### A3 — no chunked bodies

The response is read to EOF. `Content-Length` trims the body when present.
