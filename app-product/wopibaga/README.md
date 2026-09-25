# wopibaga

WOPI host helpers for Collabora Online and OnlyOffice. The package decides
locks and builds CheckFileInfo JSON. The application stores files, users,
and lock rows.

| | |
|--|--|
| **sandak** | `wopibaga` **0.1.0** |
| **Deps** | std |

```baga
let act = wopi_apply("LOCK", "", 0, 100, "abc")
// act.status == 200, act.store == 1

wopi_check_json("a.docx", "1", 12, "2", "hash", "Ada", 1)
```

Not in this package: X-WOPI-Proof, PutRelative, and the bytes of the file.

## License

[MIT](LICENSE) — Copyright (c) 2026 Dim Gigov.
