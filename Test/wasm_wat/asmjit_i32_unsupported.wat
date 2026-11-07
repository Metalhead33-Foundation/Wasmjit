;; AsmJIT supports scalar and v128 block parameters now, but a v128 block
;; *result* is still outside the scalar result subset. This remains a durable
;; "unsupported" probe for the rollback test: the body lowering fails and no
;; executable allocation may survive.
(module
  (func (export "already_compiled") (result i32)
    i32.const 42)
  (func (export "unsupported") (result i32)
    block (result v128)
      v128.const i32x4 0 0 0 0
    end
    drop
    i32.const 1))
