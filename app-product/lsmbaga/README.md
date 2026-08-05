# lsmbaga → **rocksbaga**

This package name is **deprecated** as of R10 (2026-08-05).

The storage engine lives at **[`../rocksbaga/`](../rocksbaga/)**.

```baga
// preferred (layered)
import "rocksbaga/db/engine.baga"
import "rocksbaga/net/server.baga"
// or short re-exports
import "rocksbaga/engine.baga"
```

See `../rocksbaga/ARCHITECTURE.md`. Shim files here re-export rocksbaga
root paths so old `import "lsmbaga/…"` still compiles.
