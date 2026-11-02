;; CM-21: a component that exports a value, an instance and a type through the
;; public facade.
(component
  (type $t u32)
  (core module $m (func (export "f") (result i32) (i32.const 7)))
  (core instance $ci (instantiate $m))
  (alias core export $ci "f" (core func $f))
  (type $ft (func (result u32)))
  (canon lift (core func $f) (func $lifted (type $ft)))
  (instance $i (export "f" (func $lifted)))
  (export "t" (type $t))
  (export "i" (instance $i))
)
