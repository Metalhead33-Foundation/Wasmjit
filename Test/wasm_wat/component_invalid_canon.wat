;; canon lift must name a component function type, not a record type
(component
  (core module $m (func (export "f")))
  (core instance $i (instantiate $m))
  (alias core export $i "f" (core func $f))
  (type $t (record (field "x" u32)))
  (canon lift (core func $f) (func (type $t)))
)