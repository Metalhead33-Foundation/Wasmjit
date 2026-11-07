(module
  (func (export "already_compiled") (result i32)
    i32.const 42)
  ;; A multi-value BLOCK is still refused by AsmJIT's body lowering whatever the
  ;; calling form is, so it is a durable "unsupported" probe for the rollback test.
  ;; The previous probe used a v128 signature, which the frame form now handles
  ;; (docs/backends/PRODUCTION_GAPS.md section 12).
  (func (export "unsupported") (result i32)
    block (result i32 i32)
      i32.const 1
      i32.const 2
    end
    drop)
)
