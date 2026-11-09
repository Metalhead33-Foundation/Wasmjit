;; A durable AsmJIT refusal: br_table to a multi-result function label. The
;; first function must still compile before the second one is refused, which is
;; what the allocation-rollback test checks.
(module
  (func (export "already_compiled") (result i32)
    i32.const 42)
  (func (export "unsupported") (result i32 i32)
    i32.const 1
    i32.const 2
    i32.const 0
    br_table 0))
