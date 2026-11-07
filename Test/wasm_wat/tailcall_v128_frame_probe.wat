;; v128 frame-form cross-function trampoline: three vector parameters force the
;; staging-frame ABI. Each link copies the vectors through the 16-byte uniform
;; slot into `TailCallState::frame`; the 1,000,000-link chain must run in
;; constant native stack and preserve the scalar counter.
(module
  (func $even (param i32 v128 v128 v128) (result i32)
    local.get 0
    i32.eqz
    if (result i32)
      i32.const 1
    else
      local.get 0
      i32.const 1
      i32.sub
      local.get 1
      local.get 2
      local.get 3
      return_call $odd
    end)

  (func $odd (param i32 v128 v128 v128) (result i32)
    local.get 0
    i32.eqz
    if (result i32)
      i32.const 0
    else
      local.get 0
      i32.const 1
      i32.sub
      local.get 1
      local.get 2
      local.get 3
      return_call $even
    end)

  (func (export "run") (result i32)
    i32.const 1000000
    v128.const i32x4 1 2 3 4
    v128.const i32x4 5 6 7 8
    v128.const i32x4 9 10 11 12
    call $even))
