;; Function-reference tail calls: `run` enters through `call_ref`, and each link
;; uses `return_call_ref` on a `ref.func` value. A backend that resolves the
;; Callable and sees its thunk must loop in constant native stack.
(module
  (type $t (func (param i32) (result i32)))
  (elem declare func $even $odd)

  (func $even (param i32) (result i32)
    local.get 0
    i32.eqz
    if (result i32)
      i32.const 1
    else
      local.get 0
      i32.const 1
      i32.sub
      ref.func $odd
      return_call_ref $t
    end)

  (func $odd (param i32) (result i32)
    local.get 0
    i32.eqz
    if (result i32)
      i32.const 0
    else
      local.get 0
      i32.const 1
      i32.sub
      ref.func $even
      return_call_ref $t
    end)

  (func (export "even0") (result i32)
    i32.const 0
    ref.func $even
    call_ref $t)

  (func (export "run") (result i32)
    i32.const 1000000
    ref.func $even
    call_ref $t))
