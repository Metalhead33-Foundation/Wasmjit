;; Superseded Phase-3 exception handling: a `delegate` that skips a *final-form*
;; catch.
;;
;;     wat2wasm --enable-exceptions delegate_skip_table.wat -o delegate_skip_table.wasm
;;
;; The delegating try sits inside a `try_table` that catches and delegates *past*
;; it to the enclosing legacy try, so the `try_table` must not catch. Since LE-9
;; mixed bodies load, this is the case that could otherwise slip through: the
;; rewrite has to treat a final-form `try_table` with clauses as an intervening
;; catch too (docs/proposals/LEGACY_EXCEPTIONS.md LE8). Depth 2, not 1: label 1
;; *is* the try_table, and delegating to a frame is not skipping it.
(module
  (tag $e (param i32))
  (func $skip_table
    (block $h
      (try
        (do
          (try_table (catch_all $h)
            (try
              (do (i32.const 9) (throw $e))
              (delegate 2))))
        (catch $e
          drop))))
  (export "skip_table" (func $skip_table)))
