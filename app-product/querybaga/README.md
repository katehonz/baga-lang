# querybaga

**URL query / form** parse and encode (closes jwt G7-style gaps).

```baga
let q = query_parse("a=1&b=hello%20x")
query_get(q, "b")                    // "hello x"
query_from_path("/api?x=1")          // query map
query_encode(q)                      // "a=1&b=hello%20x"
```
