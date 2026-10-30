;; docs/proposals/PARSER_PROPOSALS.md PP-2: Relaxed Dead-Code Validation.
;; Every instruction after `unreachable` is in dead code, so its type-stack
;; checks (and its pops/pushes) are skipped. Immediate/bounds checks remain.
;; The module is accepted and traps only when a function is actually called.
(module
  (memory 1)

  (func $callee (param i32) (result i32)
    local.get 0)

  ;; A sequence that would fail every conservative type check.
  (func (export "dead") (result i32)
    unreachable
    i32.add            ;; no operands
    i64.mul            ;; no operands
    ref.is_null        ;; pops a non-reference
    drop               ;; no operand
    select             ;; three missing operands
    i32.store          ;; two missing operands
    call $callee       ;; missing argument
    memory.grow        ;; missing delta
    i32.const 7        ;; dead push (must not materialise)
    i32.add)

  ;; Entering a block from dead code starts a fresh, reachable frame with the
  ;; block's parameters, so the body below is ordinary reachable code.
  (func (export "dead-block") (param i32) (result i32)
    unreachable
    (block (param i32) (result i32)
      i32.const 1
      i32.add))

  ;; The same shape in an `if`, whose `else` frame is reachable as well.
  (func (export "dead-if") (param i32) (result i32)
    unreachable
    (if (param i32) (result i32)
      (then
        i32.const 1
        i32.add)
      (else
        i32.const 2
        i32.add))))
