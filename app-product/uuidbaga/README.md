# uuidbaga

**UUID v4** (RFC 4122).

```baga
let id = uuid_v4()?          // "550e8400-e29b-41d4-a716-446655440000"
uuid_ok(id)                  // 1
uuid_version(id)             // 4
uuid_parse(id) catch !BadUuid(s) => s   // lowercase, or the bad input
```
