;; Lower the lifted function back to a core callable through an ffi closure.
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
  (canon lower (func $lifted) (core func $lowered))
)
