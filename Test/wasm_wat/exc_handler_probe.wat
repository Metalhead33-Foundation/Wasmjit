;; Item 4, probe 13: a `throw` that a handler CATCHES. This asks whether the
;; handler path exists at all (docs/TESTING.md section 8).
;;
;; The try_table is void: its body ends in a throw, so it can only fall through
;; with a declared result if the module is invalid (wasm-tools validate caught
;; this; the catch branch targets $h with the payload, and the normal path
;; produces the block's 0).
(module
  (tag $e (param i32))
  (func (export "caught") (result i32)
    block $h (result i32)
      try_table (catch $e $h)
        i32.const 42
        throw $e
      end
      i32.const 0
    end))
