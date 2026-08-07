(module
  (memory (export "mem") 1)
  (func (export "size") (result i32)
    memory.size)
  (func (export "load") (param i32) (result i32)
    local.get 0
    i32.load8_u)
  (func (export "store") (param i32 i32)
    local.get 0
    local.get 1
    i32.store8)
  (func (export "load32") (param i32) (result i32)
    local.get 0
    i32.load)
  (func (export "store32") (param i32 i32)
    local.get 0
    local.get 1
    i32.store)
)
