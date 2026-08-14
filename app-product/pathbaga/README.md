# pathbaga

Universal **path** helpers (POSIX `/` style). Pure Baga.

```baga
path_join("/var", "log/app")   // "/var/log/app"
path_join_all(parts)           // fold of path_join
path_basename("/a/b.c")        // "b.c"
path_dirname("/a/b.c")         // "/a"
path_ext("x.baga")             // ".baga"
path_stem("x.baga")            // "x"
path_is_abs("/tmp")            // 1
```
