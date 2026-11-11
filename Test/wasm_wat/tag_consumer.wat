;; Item 4, probe 21b: a module that imports the tag and CATCHES a throw raised in the
;; provider instance (docs/TESTING.md section 15).
;;
;; The try_table carries the imported function's i32 result, so it is the block's
;; only result. An extra `i32.const 0` after it made the module invalid
;; (`wasm-tools validate`: values remaining on stack at end of block); the catch
;; branch supplies the payload to $h.
(module
  (import "provider" "e" (tag $e (param i32)))
  (import "provider" "thrown" (func $thrown (result i32)))
  (func (export "caught") (result i32)
    block $h (result i32)
      try_table (result i32) (catch $e $h)
        call $thrown
      end
    end))
