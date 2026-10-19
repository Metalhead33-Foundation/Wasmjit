;; A parent component instantiates a nested component and aliases its export.
(component
  (component $child
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
    (canon lift (core func $add) (func $f (type $ft)))
    (export "add" (func $f))
  )
  (instance $ci (instantiate $child))
  (alias export $ci "add" (func $add))
  (export "add" (func $add))
)
