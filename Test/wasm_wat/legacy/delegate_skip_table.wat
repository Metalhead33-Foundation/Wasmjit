;; Superseded Phase-3 exception handling: a `delegate` that skips a *final-form*
;; catch.
;;
;;     wat2wasm --enable-exceptions delegate_skip_table.wat -o delegate_skip_table.wasm
;;
;; The delegating try sits inside a `try_table` that catches and delegates *past*
;; it to the enclosing legacy try, so the `try_table` must not catch. Since LE-9
;; mixed bodies load, this is the case that could otherwise slip through: the
;; rewrite has to treat a final-form `try_table` with clauses as an intervening
;; catch too (docs/proposals/LEGACY_EXCEPTIONS.md LE-8). Depth 2, not 1: label 1
;; *is* the try_table, and delegating to a frame is not skipping it.
;;
;; `$mark` makes the routing observable: 1 means the legacy `catch $e` took the
;; exception (correct), 0 means the `try_table` caught what the delegate sent
;; past it.
(module
  (tag $e (param i32))
  (global $mark (mut i32) (i32.const 0))
  (func $skip_table (result i32)
    (block $h
      (try
        (do
          (try_table (catch_all $h)
            (try
              (do (i32.const 9) (throw $e))
              (delegate 2))))
        (catch $e
          (drop)
          (global.set $mark (i32.const 1)))))
    (global.get $mark))
  (export "skip_table" (func $skip_table)))
