(module
  ;; multi-value: returns (a / b, a % b) unsigned
  (func (export "divmod") (param $a i32) (param $b i32) (result i32 i32)
    local.get $a
    local.get $b
    i32.div_u
    local.get $a
    local.get $b
    i32.rem_u)

  ;; f64 params + f64 result: sqrt(a*a + b*b)
  (func (export "norm") (param $a f64) (param $b f64) (result f64)
    local.get $a
    local.get $a
    f64.mul
    local.get $b
    local.get $b
    f64.mul
    f64.add
    f64.sqrt)

  ;; i64 beyond i32 range
  (func (export "add64") (param $a i64) (param $b i64) (result i64)
    local.get $a
    local.get $b
    i64.add)
)
