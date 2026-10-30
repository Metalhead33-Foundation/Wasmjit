;; Minimal executable component: one core module, one core instance, a lifted
;; flat scalar function, and its component export.
(component
  (core module $m
    (func (export "add") (param i32 i32) (result i32)
      (local.get 0)
      (local.get 1)
      i32.add
    )
  )
  (core instance $i (instantiate $m))
  (alias core export $i "add" (core func $add))
  (type $ft (func (param "a" u32) (param "b" u32) (result u32)))
  (canon lift (core func $add) (func $lifted (type $ft)))
  (export "add" (func $lifted))
)
