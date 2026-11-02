;; CM19: stackful async lift. The core callee reports its result through
;; `task.return` and returns no core results; the runtime runs one task and
;; lifts the stored flat result back to the caller.
(component
  (core module $M
    (import "" "task.return" (func $task.return (param i32)))
    (func (export "run") (param i32)
      (call $task.return (local.get 0))
    )
  )
  (canon task.return (result u32) (core func $task.return))
  (core instance $ci (instantiate $M
    (with "" (instance (export "task.return" (func $task.return))))))
  (func (export "run") async (param "x" u32) (result u32)
    (canon lift (core func $ci "run") async))
)
