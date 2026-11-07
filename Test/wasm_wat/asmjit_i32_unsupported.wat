;; AsmJIT supports scalar block parameters and multi-value block results now,
;; but a v128 block parameter is still outside the register-form subset. This
;; remains a durable "unsupported" probe for the rollback test: the body
;; lowering fails and no executable allocation may survive.
(module
  (func (export "already_compiled") (result i32)
    i32.const 42)
  (func (export "unsupported") (result i32)
    v128.const i32x4 0 0 0 0
    (block (param v128) (result i32)
      drop
      i32.const 1))
)
