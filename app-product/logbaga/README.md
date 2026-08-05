# logbaga

Structured **JSON lines** on stderr (Track **C3**).

```baga
log_info("server up")?
log_info_req("handled", "r-42")?
log_error("boom")?
```

Fields: `ts` (wall ms), `level`, `msg`, optional `req_id`.
Uses `std/json` `json_escape` for safe string values.
