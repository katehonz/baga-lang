# davbaga

WebDAV protocol helpers for Baga (RFC 4918): Depth, Destination, Basic,
If-Match, and multistatus XML. Storage and ACLs stay in the application.

```baga
import "davbaga/dav.baga"

dav_depth("1")                 // 1; "" and "infinity" are -1; junk is -2
dav_url_path("http://h/dav/2/a")
dav_response("/dav/2/a", "a", 0, 3, "text/plain", "abc", "")
dav_multistatus(inner)
```

LOCK, PROPPATCH and the tagged `If` header are not in this package yet.
