;; CM-17: a component definition exported for another component to import.
(component
  (core module $m
    (func (export "f") (result i32) (i32.const 42)))
  (core instance $i (instantiate $m))
  (alias core export $i "f" (core func $f))
  (type $ft (func (result u32)))
  (canon lift (core func $f) (func $lifted (type $ft)))
  (export "answer" (func $lifted))
)
