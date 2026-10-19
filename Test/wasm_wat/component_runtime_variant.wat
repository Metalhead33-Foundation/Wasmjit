;; A variant whose i32/f32 payloads use the Canonical ABI join (i32).
(component
  (core module $m
    (memory (export "memory") 1)
    (func (export "echo") (param i32 i32) (result i32)
      (i32.store (i32.const 1024) (local.get 0))
      (i32.store (i32.const 1028) (local.get 1))
      (i32.const 1024)
    )
  )
  (core instance $i (instantiate $m))
  (alias core export $i "echo" (core func $echo))
  (alias core export $i "memory" (core memory $mem))
  (type $v (variant (case "i" u32) (case "f" float32)))
  (export $v2 "v" (type $v))
  (type $ft (func (param "v" $v2) (result $v2)))
  (canon lift (core func $echo) (memory $mem) (func $f (type $ft)))
  (export "echo" (func $f))
)
