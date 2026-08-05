# lsmbaga → **rocksbaga**

This package name is **deprecated** as of R10 (2026-08-05).

The storage engine lives at **[`../rocksbaga/`](../rocksbaga/)**.

```baga
// preferred
import "rocksbaga/engine.baga"
import "rocksbaga/server.baga"
```

Shim files under this directory re-export the same modules so old
`import "lsmbaga/…"` paths keep compiling during migration.
