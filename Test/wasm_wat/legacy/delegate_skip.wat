;; Superseded Phase-3 exception handling: `delegate` that *skips* an intervening
;; catch (docs/proposals/LEGACY_EXCEPTIONS.md LE-8).
;;
;;     wat2wasm --enable-exceptions delegate_skip.wat -o delegate_skip.wasm
;;
;; `skip` delegates *past* the middle try, whose `catch_all` would otherwise
;; catch: the exception must reach the outer `catch $e`, so the answer is the
;; payload (9), never the middle arm's 1.
;;
;; `to_caller` delegates *past* its enclosing `catch $e` to a label that is not
;; a `try` — the shape clang emits for a cleanup pad. The exception must leave
;; the function as the caller's pending exception instead of being caught; the
;; `block` is the delegate's target and, like any non-`try` label, means "out".
(module
  (tag $e (param i32))

  (func $skip (result i32)
    (try (result i32)
      (do
        (try (result i32)
          (do
            (try (result i32) (do (i32.const 9) (throw $e)) (delegate 2)))
          (catch_all (i32.const 1))))
      (catch $e)))

  (func $to_caller (param i32) (result i32)
    (block (result i32)
      (try (result i32)
        (do
          (try (result i32) (do (i32.const 5) (throw $e)) (delegate 2)))
        (catch $e (drop) (i32.const 0)))))

  ;; A delegate that sits *inside* a handler: the try owning that handler cannot
  ;; catch what its arms raise, so nothing has to be skipped and the exception
  ;; must leave the function even though `delegate 2` names the enclosing block.
  (func $from_arm (param i32) (result i32)
    (try (result i32)
      (do (i32.const 9) (throw $e))
      (catch $e
        (drop)
        (block (result i32)
          (try (result i32) (do (i32.const 77) (throw $e)) (delegate 2))))))

  (export "skip" (func $skip))
  (export "to_caller" (func $to_caller))
  (export "from_arm" (func $from_arm)))
