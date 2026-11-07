;; Cross-function trampoline probe: even/odd alternate through `return_call`.
;; With call + return this exhausts the guest stack guard; a backend with a real
;; per-function driver returns 1 (1,000,000 is even) in constant native stack.
(module
  (func $even (param i32) (result i32)
    local.get 0
    i32.eqz
    if (result i32)
      i32.const 1
    else
      local.get 0
      i32.const 1
      i32.sub
      return_call $odd
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
      return_call $even
    end)

  (func (export "even0") (result i32)
    i32.const 0
    call $even)

  (func (export "run") (result i32)
    i32.const 1000000
    call $even))
