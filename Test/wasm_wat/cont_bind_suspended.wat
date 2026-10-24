;; `cont.bind` applied to a suspended continuation: the generator's next
;; resume value is supplied by the binding. The original continuation type
;; (`[] -> i32`) becomes `[i32] -> i32` after the suspension, and binding the
;; i32 returns the original `[] -> i32` shape.
(module
  (type $ft (func (result i32)))
  (type $ct (cont $ft))
  (type $fts (func (param i32) (result i32)))
  (type $cts (cont $fts))
  (tag $yield (param i32) (result i32))
  (type $handler (func (result i32 (ref null cont))))

  (func $gen (type $ft)
    i32.const 7
    suspend $yield
    i32.const 1
    i32.add)

  (elem declare func $gen)

  (func (export "run") (result i32)
    (local $c (ref null cont))
    (block $on_yield (type $handler)
      ref.func $gen
      cont.new $ct
      resume $ct (on $yield 0)
      return)
    local.set $c
    drop
    i32.const 10
    local.get $c
    cont.bind $cts $ct
    resume $ct))
