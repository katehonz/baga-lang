# std/json

JSON parser + serializer, written in pure Baga.

## Usage

```baga
import "std/json/json.baga"

let d = json_parse("{\"a\": [1, 2, 3]}")
let r = json_root(d)
```

## Representation

A parsed document is a `JsonDoc`. Nodes are `i64` indices into parallel
Vec pools; children of arrays and objects live in a flat `kids` pool.
Object pairs occupy two consecutive entries (key node, value node).

```baga
struct JsonDoc {
    root: i64,
    tags: Vec<i64>,
    nums: Vec<f64>,
    strs: Vec<str>,
    kids: Vec<i64>,
    kfrom: Vec<i64>,
    klen: Vec<i64>
}
```

Node tags:

| tag | meaning |
|-----|---------|
| -1  | error   |
| 0   | null    |
| 1   | false   |
| 2   | true    |
| 3   | number  |
| 4   | string  |
| 5   | array   |
| 6   | object  |

## API

```baga
fn json_parse(s: str) -> JsonDoc
fn json_root(d: JsonDoc) -> i64
fn json_tag(d: JsonDoc, n: i64) -> i64
fn json_num(d: JsonDoc, n: i64) -> f64
fn json_str(d: JsonDoc, n: i64) -> str
fn json_count(d: JsonDoc, n: i64) -> i64       // array elements / object pairs
fn json_at(d: JsonDoc, n: i64, i: i64) -> i64  // array element node
fn json_key(d: JsonDoc, n: i64, i: i64) -> str // object key at pair i
fn json_val(d: JsonDoc, n: i64, i: i64) -> i64 // object value node at pair i
fn json_get(d: JsonDoc, n: i64, key: str) -> i64 // value node for key, or -1
fn json_serialize(d: JsonDoc, n: i64) -> str
```

## Notes

- Numbers serialize from their raw text, so `json_serialize` is lossless
  (e.g. `2.50` stays `2.50`); `json_num` gives the parsed `f64` value.
- `\uXXXX` escapes decode BMP code points only (no surrogate pairs).
- The parser is lenient: unterminated strings or containers still yield
  nodes. A completely unparseable document yields a root node with tag
  `-1`.
- `json_get` does a linear scan; first match wins.

Effects: none (pure). Memory: leak-tolerant.
