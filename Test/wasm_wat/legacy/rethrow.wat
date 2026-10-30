;; Superseded Phase-3 exception handling: `rethrow`, in the three shapes that
;; matter (docs/proposals/LEGACY_EXCEPTIONS.md LE-2/LE-4).
;;
;;     wat2wasm --enable-exceptions rethrow.wat -o rethrow.wasm
;;
;; * `cleanup` is clang's cleanup pad: catch_all + rethrow, no payload.
;; * `same` rethrows from a payload arm and is caught in the same function.
;; * `outer` calls `payload`, which rethrows across the function boundary.
(module
  (tag $e (param i32))

  ;; Nothing in the module catches this, so it leaves the module.
  (func $cleanup (param i32) (result i32)
    (try (result i32)
      (do (i32.const 7) (throw $e))
      (catch_all (rethrow 0))))

  (func $payload (param i32) (result i32)
    (try (result i32)
      (do (i32.const 9) (throw $e))
      (catch $e (rethrow 0))))

  ;; The same-function case: the rethrow lands in the outer arm.
  (func $same (result i32)
    (try (result i32)
      (do
        (try (result i32)
          (do (i32.const 5) (throw $e))
          (catch $e (rethrow 0))))
      (catch $e (i32.const 1) (i32.add))))

  ;; The cross-function case: `$payload` rethrows into this try.
  (func $outer (result i32)
    (try (result i32)
      (do (call $payload (i32.const 0)))
      (catch $e (i32.const 40) (i32.add))))

  ;; A void catch_all that rethrows and is caught by a void arm.
  (func $voidpath (result i32)
    (try (result i32)
      (do
        (try (result i32)
          (do (i32.const 5) (throw $e))
          (catch_all (rethrow 0))))
      (catch_all (i32.const 3))))

  (export "cleanup" (func $cleanup))
  (export "payload" (func $payload))
  (export "same" (func $same))
  (export "outer" (func $outer))
  (export "voidpath" (func $voidpath)))
