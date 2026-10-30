;; Superseded Phase-3 exception handling mixed with the final form.
;;
;;     wat2wasm --enable-exceptions mixed.wat -o mixed.wasm
;;
;; A body may use both encodings: a `try_table` whose clauses keep their final
;; spelling while the surrounding try is still the superseded one. The rewrite
;; inserts frames for every legacy try (a merge block, one landing block per
;; arm, the `try_table` itself), so every clause label has to be renumbered
;; exactly like a branch (docs/proposals/LEGACY_EXCEPTIONS.md LE-9). Before
;; LE-9 the whole body was refused by name.
(module
  (tag $e (param i32))

  ;; A final try_table inside a legacy try, whose clause branches to a block
  ;; *outside* the legacy try: correct output is 42, a lost renumbering lands
  ;; on a legacy arm landing instead.
  (func $mixed (result i32)
    (block $out (result i32)
      (try (result i32)
        (do
          (try_table (result i32) (catch $e 1)
            (i32.const 42)
            (throw $e)))
        (catch_all (i32.const 7)))))
  (export "mixed" (func $mixed))

  ;; An ordinary branch out of the try_table's body, across the legacy try:
  ;; the try_table frame has to be counted or `br 2` hits the wrong label.
  (func $branch (param i32) (result i32)
    (block $out (result i32)
      (try (result i32)
        (do
          (try_table (result i32) (catch $e 1)
            (br_if 2 (i32.const 11) (local.get 0))
            (i32.const 42)
            (throw $e)))
        (catch_all (i32.const 7)))))
  (export "branch" (func $branch))

  ;; A final try_table inside a legacy *arm*: the arm's landing block sits
  ;; between the clause label and the try it names.
  (func $arm (result i32)
    (try (result i32)
      (do (throw $e (i32.const 5)))
      (catch $e
        drop
        (try_table (result i32) (catch $e 0)
          (throw $e (i32.const 9))))))
  (export "arm" (func $arm)))
