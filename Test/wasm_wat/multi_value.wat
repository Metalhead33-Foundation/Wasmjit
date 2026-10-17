;; Regression for the multi-result ABI (docs/proposals/MULTI_VALUE.md MV5).
;;
;; A multi-result function does not return an aggregate by value: the caller
;; passes a result-buffer pointer as a trailing JIT parameter and the function
;; returns void. Before that change LibJIT's aggregate-return ABI read the
;; hidden buffer pointer from an unwritten frame slot, so a JIT-to-JIT call
;; wrote its results through 0 or through garbage (the `segfault at 8` /
;; general-protection-fault family in call.wast, if.wast and fac.wast).
(module
  (type $swap_t (func (param i32 i32) (result i32 i32)))

  (func $swap (type $swap_t)
    (local.get 1) (local.get 0))

  (func $pick (param i64 i64) (result i64 i32)
    (local.get 0) (i32.wrap_i64 (local.get 1)))

  ;; call.wast's `as-mixed-operands`: consume two results on the stack.
  (func (export "mixed") (param i32 i32) (result i32)
    (call $swap (local.get 0) (local.get 1))
    (i32.const 5)
    (i32.add)
    (i32.mul))

  ;; call.wast's `as-call-all-operands`: feed one multi-result call into another.
  (func (export "nested") (result i32)
    (call $swap (call $swap (i32.const 3) (i32.const 4)))
    (i32.add))

  ;; if.wast's `add64_u_with_carry` shape: an i64/i32 tuple, not 2 x i32.
  (func (export "mixed64") (param i64 i32) (result i64)
    (call $pick (local.get 0) (i64.extend_i32_u (local.get 1)))
    (drop))

  ;; if.wast's `params-id-break`: an if with parameters and no else.
  (func (export "params_id_break") (param i32) (result i32)
    (i32.const 1)
    (i32.const 2)
    (if (param i32 i32) (result i32 i32) (local.get 0) (then (br 0)))
    (i32.add))

  ;; A multi-result callee reached through call_indirect.
  (table 1 funcref)
  (elem (i32.const 0) $swap)
  (func (export "indirect") (param i32 i32) (result i32)
    (call_indirect (type $swap_t) (local.get 0) (local.get 1) (i32.const 0))
    (i32.add))

  ;; The same function exported directly, so the host-side ABI (a trailing
  ;; result-buffer pointer) can be exercised without an internal call.
  (func (export "swap_export") (type $swap_t)
    (local.get 1) (local.get 0)))
