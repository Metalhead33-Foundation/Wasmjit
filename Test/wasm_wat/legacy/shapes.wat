;; Superseded Phase-3 exception handling: control-flow shapes the rewrite must
;; remap (docs/proposals/LEGACY_EXCEPTIONS.md LE-3/LE-5):
;;
;;     wat2wasm --enable-exceptions shapes.wat -o shapes.wasm
;;
;; `b1`..`b3` branch *out of* a try body or arm, and to an inner block.
;; `d1`/`d2` delegate to an enclosing frame, with and without a plain block in
;; between. The refusal case lives in `delegate_skip.wat`.
(module
  (tag $e (param i32))

  ;; Branch out of the body to an enclosing block (skips the arms).
  (func $b1 (result i32)
    (block $out (result i32)
      (try (result i32)
        (do (br $out (i32.const 11)))
        (catch_all (i32.const 33)))))

  ;; Branch out of an arm to an enclosing block, carrying the payload.
  (func $b2 (result i32)
    (block $out (result i32)
      (try (result i32)
        (do (i32.const 5) (throw $e))
        (catch $e (br $out)))))

  ;; Branch to a block opened inside the body.
  (func $b3 (result i32)
    (try (result i32)
      (do (block $inner (result i32) (i32.const 7) (br $inner)))
      (catch_all (i32.const 1))))

  ;; A branch table inside the body: every label needs the same remap.
  (func $b4 (param i32) (result i32)
    (block $a
      (block $b
        (try
          (do
            (block $c
              (br_table $a $b $c (local.get 0)))
            (return (i32.const 20)))
          (catch_all (nop))))
      (return (i32.const 21)))
    (i32.const 22))

  ;; delegate to the enclosing try.
  (func $d1 (result i32)
    (try (result i32)
      (do (try (result i32) (do (i32.const 9) (throw $e)) (delegate 1)))
      (catch $e)))

  ;; delegate past a plain block: the block is not a catch frame.
  (func $d2 (result i32)
    (try (result i32)
      (do (block (result i32)
            (try (result i32) (do (i32.const 9) (throw $e)) (delegate 2))))
      (catch $e)))

  (export "b1" (func $b1))
  (export "b2" (func $b2))
  (export "b3" (func $b3))
  (export "b4" (func $b4))
  (export "d1" (func $d1))
  (export "d2" (func $d2)))
