;; CM16: `callback` is only allowed on `canon lift`, not `canon lower`.
(component
  (import "host" (func $host async (param "x" u32) (result u32)))
  (core module $M
    (func (export "cb") (param i32 i32 i32) (result i32) (i32.const 0)))
  (core instance $ci (instantiate $M))
  (alias core export $ci "cb" (core func $cb))
  (canon lower (func $host) async (callback $cb)
    (core func $lowered))
)
