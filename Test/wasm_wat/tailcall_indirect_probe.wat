;; Indirect cross-function trampoline: even/odd alternate through
;; `return_call_indirect` on a two-entry funcref table. With call + return this
;; exhausts the guest stack; a backend whose driver sees the resolved callable's
;; tailEntry loops in constant native stack.
(module
  (type $t (func (param i32) (result i32)))
  (table 2 funcref)
  (elem (i32.const 0) $even $odd)

  (func $even (param i32) (result i32)
    local.get 0
    i32.eqz
    if (result i32)
      i32.const 1
    else
      local.get 0
      i32.const 1
      i32.sub
      i32.const 1
      return_call_indirect (type $t)
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
      i32.const 0
      return_call_indirect (type $t)
    end)

  (func (export "run") (result i32)
    i32.const 1000000
    call $even))
