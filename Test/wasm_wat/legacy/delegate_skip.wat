;; Superseded Phase-3 exception handling: the one shape the rewrite refuses.
;;
;;     wat2wasm --enable-exceptions delegate_skip.wat -o delegate_skip.wasm
;;
;; The innermost try delegates *past* the middle try, whose arm would otherwise
;; catch. Honouring that skip needs the enclosing try's clauses to be split per
;; region (docs/proposals/LEGACY_EXCEPTIONS.md LE8), so the rewrite refuses it by
;; name rather than approximating it.
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
  (export "skip" (func $skip)))
