;; v128 function-reference frame calls: a scalar counter and a vector
;; parameter force the staging-frame ABI. `run` enters through `call_ref`; each
;; link uses `return_call_ref`, so the frame driver copies the vector through
;; TailCallState::frame on every iteration.
(module
  (type $t (func (param i32 v128 v128) (result i32)))
  (elem declare func $even $odd)

  (func $even (param i32 v128 v128) (result i32)
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
      ref.func $odd
      return_call_ref $t
    end)

  (func $odd (param i32 v128 v128) (result i32)
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
      ref.func $even
      return_call_ref $t
    end)

  (func (export "run") (result i32)
    i32.const 1000000
    v128.const i32x4 1 2 3 4
    v128.const i32x4 5 6 7 8
    ref.func $even
    call_ref $t))
