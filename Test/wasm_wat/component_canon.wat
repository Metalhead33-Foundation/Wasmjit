;; Exercises core module/instance, core export alias, component func type and
;; canon lift + export.
(component
  (core module $m
    (func (export "f") (param i32) (result i32)
      (local.get 0)
    )
  )
  (core instance $i (instantiate $m))
  (alias core export $i "f" (core func $f))
  (type $ft (func (param "x" u32) (result u32)))
  (canon lift (core func $f) (func $lifted (type $ft)))
  (export "f" (func $lifted))
)