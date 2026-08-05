# bufbaga

**String builder** — push chunks, join once (mdbaga M1 / template builders).

```baga
let mut b = buf_new()
b = buf_push(b, "<h1>")
b = buf_push(b, title)
b = buf_push(b, "</h1>")
let html = buf_str(b)
```
