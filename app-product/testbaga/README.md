# testbaga

A **minimal test assertion library** for Baga — apps-roadmap **№8**.
Replaces the copy-pasted `fn check` in `*_test.baga` files. Multi-file
**discovery** is a thin shell driver (`scripts/baga-test`) until the language
has readdir + process spawn.

## API

### Fail-fast (drop-in for `check`)

```baga
import "testbaga/assert.baga"

fn main() {
    assert_true("flag", 1 < 2)
    assert_eq("sum", 2 + 2, 4)          // generic (M21 + Show)
    assert_eq("hi", concat("h", "i"), "hi")
    assert_ne_str("diff", "a", "b")
    test("named", my_case)              // L5: fn() -> i64, 0 = pass
}
```

On failure: prints `FAIL name` (+ diff for eq), `exit(1)`.

### Suite (continue on failure)

```baga
let mut s = suite_new("my")
s = suite_eq_i64(s, "a", 1, 1)
s = suite_eq_str(s, "b", "x", "y")   // soft fail
return suite_finish(s)               // summary; exit code 0/1
```

## Run

```bash
cd app-product/testbaga
BAGA=../../baga sandak build
../../baga -I ../.. -I .. demo.baga

# dogfood unit test
./baga -I . -I app-product tests/testbaga_test.baga

# discovery runner (shell; see gaps)
./scripts/baga-test tests/testbaga_test.baga tests/std/sort_test.baga
./scripts/baga-test tests/std          # all std *_test.baga
./scripts/baga-test                    # all under tests/
```

`sort_test` is the first std suite migrated to `testbaga` (dogfood).

## Honest limits

See [`gaps.md`](gaps.md): `test("name", fn)` and generic `assert_eq<T>`
are in; no in-language readdir/process spawn — discovery is shell-side.
