;; A nested component aliases a component from its enclosing component and
;; instantiates it, exercising the runtime's outer component index space.
(component
  (component $C
    (core module $M
      (func (export "f") (result i32) i32.const 7))
    (core instance $m (instantiate $M))
    (alias core export $m "f" (core func $f))
    (func (export "f") (result u32) (canon lift (core func $f))))
  (component $Inner
    (alias outer 1 0 (component $C2))
    (instance $i (instantiate $C2))
    (alias export $i "f" (func $f))
    (export "run" (func $f)))
  (instance $inner (instantiate $Inner))
  (alias export $inner "run" (func $run))
  (export "run" (func $run)))
