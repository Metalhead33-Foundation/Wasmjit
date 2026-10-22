;; Lower a host function that accepts a canonical string.
(component
  (core module $m
    (memory (export "memory") 1)
    (func (export "realloc") (param i32 i32 i32 i32) (result i32)
      (i32.const 1024)
    )
  )
  (core instance $i (instantiate $m))
  (alias core export $i "memory" (core memory $mem))
  (alias core export $i "realloc" (core func $realloc))
  (import "host-len" (func $f (param "s" string) (result u32)))
  (canon lower (func $f) (memory $mem) (realloc $realloc) (core func $cf))
)
