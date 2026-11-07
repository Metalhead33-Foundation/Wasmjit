;; V-1 regression: the try_table declares an i32 result and the enclosing block
;; then pushes a second value before its `end`. wasm-tools validate rejects this
;; ("values remaining on stack at end of block"); the validator used to accept it
;; because a catch clause tainted the rest of the function's stack checks.
(module
  (tag $e (param i32))
  (func (export "bad") (result i32)
    block $h (result i32)
      try_table (result i32) (catch $e $h)
        i32.const 42
        throw $e
      end
      i32.const 0
    end))
