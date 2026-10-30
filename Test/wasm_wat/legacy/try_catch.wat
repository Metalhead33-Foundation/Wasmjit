;; Superseded Phase-3 exception handling: the basic shapes.
;;
;; This is a *fixture source*, not a spec script: `wasm_wat/*.wat` is compiled by
;; `wasm-tools parse` at build time, which refuses the superseded form, so these
;; live in a subdirectory and are built by hand with wabt:
;;
;;     wat2wasm --enable-exceptions try_catch.wat -o try_catch.wasm
;;
;; The `.wasm` beside this file is the committed artifact the tests load;
;; `Test/legacy-eh-probe.sh` regenerates it and records the probe matrix
;; (docs/proposals/LEGACY_EXCEPTIONS.md LE-2).
(module
  (tag $e (param i32))

  ;; A value-producing try with a tag arm and a catch_all arm.
  ;; `pick(odd)` throws the argument; the arm adds 100 to the payload.
  (func $pick (param i32) (result i32)
    (try (result i32)
      (do
        (local.get 0)
        (i32.const 1)
        (i32.and)
        (if (then (local.get 0) (throw $e)))
        (i32.const 1000))
      (catch $e
        (i32.const 100)
        (i32.add))
      (catch_all
        (i32.const -1))))

  ;; A void try: no results, and both arms produce nothing.
  (func $void_try (param i32) (result i32)
    (try
      (do
        (local.get 0)
        (if (then (i32.const 5) (throw $e))))
      (catch $e (drop))
      (catch_all (nop)))
    (i32.const 42))

  (export "pick" (func $pick))
  (export "void_try" (func $void_try)))
