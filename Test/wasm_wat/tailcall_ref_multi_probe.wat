;; Multi-result reference calls: `run` enters `$tail` through call_ref; `$tail`
;; forwards to `$pair` through return_call_ref. Both calls return (i32, i32).
(module
  (type $t (func (param i32) (result i32 i32)))
  (elem declare func $pair $tail)

  (func $pair (param i32) (result i32 i32)
    local.get 0
    local.get 0
    i32.const 1
    i32.add)

  (func $tail (param i32) (result i32 i32)
    local.get 0
    ref.func $pair
    return_call_ref $t)

  (func (export "run") (result i32)
    i32.const 5
    ref.func $tail
    call_ref $t
    i32.add))
