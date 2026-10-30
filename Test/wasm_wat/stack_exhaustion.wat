;; The guest call-stack guard (WasmBase/WasmStack.hpp, docs/SAFETY.md §2.8).
;;
;; `recurse` is plain (non-tail) recursion, so the host stack grows until the
;; engine's stack limit trips. The regression this guards against is the
;; `segfault at <rsp> ... error 6` signature the engine used to produce: the
;; kernel killing the process instead of raising the spec's
;; "call stack exhausted" trap.
(module
  (func $recurse (param $n i32) (result i32)
    (i32.add
      (call $recurse (i32.add (local.get $n) (i32.const 1)))
      (i32.const 1)))

  (export "recurse" (func $recurse))

  ;; A deep but bounded descent: it must still fit inside the usable stack, and
  ;; must keep working after the trap above unwound the unbounded recursion.
  (func $bounded (param $n i32) (result i32)
    (if (result i32) (i32.eqz (local.get $n))
      (then (i32.const 0))
      (else
        (i32.add
          (i32.const 1)
          (call $bounded (i32.sub (local.get $n) (i32.const 1)))))))

  (export "bounded" (func $bounded)))
