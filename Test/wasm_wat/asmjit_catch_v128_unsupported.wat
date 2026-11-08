;; A durable AsmJIT refusal: a v128 catch payload routed to a block result.
;; The first function must still compile before the second is refused, which is
;; what the allocation-rollback test checks.
(module
  (tag $e (param v128))
  (func (export "already_compiled") (result i32)
    i32.const 42)
  (func (export "unsupported") (result i32)
    block (result v128)
      try_table (catch $e 0)
        v128.const i32x4 0 0 0 0
        throw $e
      end
      v128.const i32x4 0 0 0 0
    end
    drop
    i32.const 1))
