;; `resume_throw` injects the `$abort` exception at the generator's suspension
;; point. The generator's own `try_table` catches it and the value returned by
;; `try_table` is the caught payload.
(module
  (type $ft (func (result i32)))
  (type $ct (cont $ft))
  (type $handler (func (result i32 (ref null cont))))
  (tag $yield (param i32) (result i32))
  (tag $abort (param i32))

  (func $gen (type $ft)
    (block $caught (result i32)
      (try_table (catch $abort $caught)
        i32.const 7
        suspend $yield
        drop
        i32.const 1)))

  (elem declare func $gen)

  (func (export "run") (result i32)
    (local $c (ref null $ct))
    (block $on_yield (type $handler)
      ref.func $gen
      cont.new $ct
      resume $ct (on $yield 0)
      return)
    local.set $c
    drop
    i32.const 99
    local.get $c
    resume_throw $ct $abort))
