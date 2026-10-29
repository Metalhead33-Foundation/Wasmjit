;; Tail calls (proposal tail-call, Phase 5 / Wasm 3.0): `return_call`,
;; `return_call_indirect` and `return_call_ref`. Semantically each is a call
;; followed by a return: the callee's results become the caller's results.
;;
;; The C++ test exercises the shallow forms for their values and the deep forms
;; for the space guarantee (TC-2/TC-5): `$count` is a native self-tail-call,
;; `mixed_sig` alternates parameter lists, and `pair_via_tail` carries a
;; multi-result tuple through the trampoline.
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

  ;; Self-recursive tail call. The JIT lowers `return_call $count` to a native
  ;; tail call, so the depth below runs in constant stack; without that
  ;; lowering it would overflow the host stack and crash.
  (func $count (export "count") (param i32) (result i32)
    (if (result i32) (i32.eqz (local.get 0))
      (then (i32.const 0))
      (else (return_call $count (i32.sub (local.get 0) (i32.const 1))))))

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
    (return_call_ref $ret_i32 (global.get $const42)))

  ;; ── TC-3: chains whose parameter lists differ ──────────────────────────────
  ;; `$mixed-a` takes (i64,i64), `$mixed-b` takes (i32,i64). The tail-call
  ;; trampoline re-enters each link through the *callee type's* thunk, so the
  ;; chain stays constant-stack; before TC-3 this fell back to call+return.
  ;; The result type matches across every link, as the validator requires.
  (func $mixed-a (param i64 i64) (result i64)
    (if (result i64) (i64.eqz (local.get 0))
      (then (local.get 1))
      (else
        (return_call $mixed-b
          (i32.wrap_i64 (i64.sub (local.get 0) (i64.const 1)))
          (local.get 1)))))

  (func $mixed-b (param i32 i64) (result i64)
    (return_call $mixed-a
      (i64.extend_i32_u (local.get 0))
      (i64.add (local.get 1) (i64.const 1))))

  (func (export "mixed_sig") (param i64) (result i64)
    (return_call $mixed-a (local.get 0) (i64.const 0)))

  ;; A multi-result tail chain: the trampoline carries the caller's result
  ;; buffer, because every link shares the caller's result tuple.
  (type $pair (func (result i32 i32)))
  (func $pair-impl (param i32) (result i32 i32)
    (i32.const 7) (local.get 0))

  (func (export "pair_via_tail") (param i32) (result i32 i32)
    (return_call $pair-impl (local.get 0))))
