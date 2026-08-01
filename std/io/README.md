# std/io — buffered Reader/Writer over fds

Built on the fd externs of std/os. Structs are by value, so the writer
functions return the updated `Writer`.

- `Reader { fd: i64 }` — buffered reader state.
- `Writer { fd: i64, buf: str }` — writer state with a string buffer.
- `reader_new(fd: i64) -> Reader` — new reader over `fd`.
- `read_line(r: Reader) -> str !IO` — one line without the trailing `"\n"`; `""` at EOF (also `""` for an empty line — use `read_n` when the distinction matters). Reads one byte at a time: simple, correct, not fast.
- `read_n(r: Reader, n: i64) -> str !IO` — up to `n` bytes (fewer at EOF).
- `writer_new(fd: i64) -> Writer` — new writer over `fd` with an empty buffer.
- `write_str(w: Writer, s: str) -> Writer` — append `s` to the buffer; returns the updated Writer. Pure.
- `flush(w: Writer) -> Writer !IO` — write the buffer to the fd and return a cleared Writer.

Effects: !IO. Memory: buffers are heap strings (leak-tolerant); no arena needed.
