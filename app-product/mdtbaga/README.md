# mdtbaga

**gRPC metadata** — Go's `metadata.MD` as a pure multimap.

```baga
let mut md = mdt_new()
md = mdt_append(md, "x-request-id", "abc")
md = mdt_append(md, "X-Request-Id", "def")   // key folded to lowercase
mdt_get(md, "x-request-id")                  // "abc" (first)
mdt_get_all(md, "x-request-id")              // ["abc","def"]
md = mdt_set(md, "authorization", "Bearer …")
mdt_from_http_pairs(req.hkeys, req.hvals)    // strip reserved headers
```

Name is **mdt** (metadata) so it does not collide with **mdbaga** (Markdown).
