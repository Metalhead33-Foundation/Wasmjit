;; CM16: `post-return` cannot be combined with `async`, even for an async
;; function type with a valid callback.
(component
  (core module $M
    (func (export "run") (param i32) (result i32) (i32.const 0))
    (func (export "cb") (param i32 i32 i32) (result i32) (i32.const 0))
    (func (export "post") (param i32)))
  (core instance $ci (instantiate $M))
  (alias core export $ci "cb" (core func $cb))
  (alias core export $ci "post" (core func $post))
  (type $ft (func async (param "x" u32) (result u32)))
  (canon lift (core func $ci "run") async (callback $cb)
    (post-return $post) (func $run (type $ft)))
  (export "run" (func $run))
)
