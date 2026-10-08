;; Tail calls (proposal tail-call, Phase 5 / Wasm 3.0): `return_call`,
;; `return_call_indirect` and `return_call_ref`. Semantically each is a call
;; followed by a return: the callee's results become the caller's results.
;;
;; The cases here stay SHALLOW (depth <= 25) so they exercise the instructions
;; without depending on the stack-space guarantee, which is not implemented yet
;; (see docs/TAILCALLS.md, TC-2/TC-3).
(module
  (type $unary (func (param i32) (result i32)))

  ;; Direct, self-recursive tail call: factorial via an accumulator.
  (func $fac-acc (param i64 i64) (result i64)
    (if (result i64) (i64.eqz (local.get 0))
      (then (local.get 1))
      (else
        (return_call $fac-acc
          (i64.sub (local.get 0) (i64.const 1))
          (i64.mul (local.get 0) (local.get 1))))))

  ;; A plain helper, used as an indirect / reference tail-call target.
  (func $inc (param i32) (result i32)
    (i32.add (local.get 0) (i32.const 1)))

  (func (export "fac_acc") (param i64 i64) (result i64)
    (return_call $fac-acc (local.get 0) (local.get 1)))

  (func (export "fac_acc_i32") (param i32 i32) (result i64)
    (return_call $fac-acc
      (i64.extend_i32_s (local.get 0))
      (i64.extend_i32_s (local.get 1))))

  (table 1 funcref)
  (elem (i32.const 0) $inc)   ;; active: $inc lives in table slot 0
  (elem declare func $inc)    ;; declarative: makes `ref.func $inc` valid

  (func (export "inc_indirect") (param i32) (result i32)
    (return_call_indirect (type $unary) (local.get 0) (i32.const 0)))

  (func (export "inc_ref") (param i32) (result i32)
    (return_call_ref $unary (local.get 0) (ref.func $inc)))

  ;; `return_call_ref` through a funcref stored in a global. This mirrors the
  ;; shape used by return_call_ref.wast and exercises the global-initializer
  ;; `ref.func` path (see Phase A notes in docs/TAILCALLS.md).
  (type $ret_i32 (func (result i32)))
  (func $const42 (result i32) (i32.const 42))
  (elem declare func $const42)
  (global $const42 (ref $ret_i32) (ref.func $const42))

  (func (export "const_via_ref") (result i32)
    (return_call_ref $ret_i32 (global.get $const42))))
