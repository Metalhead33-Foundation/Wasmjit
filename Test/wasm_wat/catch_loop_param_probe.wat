;; A caught payload is routed to a loop with a parameter: 0 enters the loop,
;; the try_table throws 42, and the catch target restarts the loop with 42.
(module
  (tag $e (param i32))

  (func (export "run") (result i32) (local $v i32)
    i32.const 0
    loop $l (param i32) (result i32)
      local.set $v
      local.get $v
      i32.eqz
      if (result i32)
        try_table (catch $e $l)
          i32.const 42
          throw $e
        end
        i32.const 0
      else
        local.get $v
      end
    end))
