;; Item 4, probe 21b: a module that imports the tag and CATCHES a throw raised in the
;; provider instance (docs/backends/CAPABILITY_PROBES.md section 15).
(module
  (import "provider" "e" (tag $e (param i32)))
  (import "provider" "thrown" (func $thrown (result i32)))
  (func (export "caught") (result i32)
    block $h (result i32)
      try_table (result i32) (catch $e $h)
        call $thrown
      end
      i32.const 0
    end))
