;; AsmJIT still refuses a block with PARAMETERS (multi-value block *results* are
;; supported now), so this remains a durable "unsupported" probe for the rollback
;; test: the body lowering fails and no executable allocation may survive.
(module
  (func (export "already_compiled") (result i32)
    i32.const 42)
  (func (export "unsupported") (result i32)
    i32.const 1
    (block (param i32) (result i32)
      i32.const 2
      i32.add))
)
