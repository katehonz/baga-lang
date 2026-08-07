(module
  (import "wasi_snapshot_preview1" "sched_yield" (func $yield (result i32)))
  (memory (export "memory") 1)
  (func (export "run") (result i32)
    call $yield)
)
