;; Item 4, probe 13: a `throw` that a handler CATCHES. D-1 showed a throw silently
;; returning on LibJIT; this asks whether the handler path exists at all
;; (docs/backends/CAPABILITY_PROBES.md section 8).
(module
  (tag $e (param i32))
  (func (export "caught") (result i32)
    block $h (result i32)
      try_table (result i32) (catch $e $h)
        i32.const 42
        throw $e
      end
      i32.const 0
    end))
