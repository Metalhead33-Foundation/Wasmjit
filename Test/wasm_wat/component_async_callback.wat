;; CM19: async lift with a callback. The core callee returns YIELD once, the
;; callback calls `task.return` and then returns EXIT. The runtime's event loop
;; drives the callback and lifts the stored result.
(component
  (core module $M
    (import "" "task.return" (func $task.return (param i32)))
    (func (export "run") (param i32) (result i32)
      (i32.const 1) ;; YIELD
    )
    (func (export "cb") (param i32 i32 i32) (result i32)
      (call $task.return (i32.const 7))
      (i32.const 0) ;; EXIT
    )
  )
  (canon task.return (result u32) (core func $task.return))
  (core instance $ci (instantiate $M
    (with "" (instance (export "task.return" (func $task.return))))))
  (alias core export $ci "cb" (core func $cb))
  (type $ft (func async (param "x" u32) (result u32)))
  (canon lift (core func $ci "run") async (callback $cb)
    (func $run (type $ft)))
  (export "run" (func $run))
)
