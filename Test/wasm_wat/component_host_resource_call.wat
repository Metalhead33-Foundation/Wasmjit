;; A component that imports an interface exporting a **resource type**, lowers its
;; constructor and one method, and lowers a `resource.drop`. This is the shape
;; every `wasi:io` import has, and the fixture that proves a host bag can own the
;; handles: `run` constructs, calls the method and drops, all in the host.
(component
  (import "dep" (instance $dep
    (export "res" (type $res (sub resource)))
    (export "[constructor]res" (func $ctor (result (own $res))))
    (export "[method]res.value" (func $value (param "self" (borrow $res))
      (result u32)))
  ))
  (alias export $dep "res" (type $res-alias))
  (alias export $dep "[constructor]res" (func $ctor-alias))
  (alias export $dep "[method]res.value" (func $value-alias))
  (canon lower (func $ctor-alias) (core func $ctor-core))
  (canon lower (func $value-alias) (core func $value-core))
  (canon resource.drop $res-alias (core func $drop-core))
  (core instance $env
    (export "ctor" (func $ctor-core))
    (export "value" (func $value-core))
    (export "drop" (func $drop-core))
  )
  (core module $m
    (import "env" "ctor" (func $ctor (result i32)))
    (import "env" "value" (func $value (param i32) (result i32)))
    (import "env" "drop" (func $drop (param i32)))
    (func (export "run") (result i32)
      (local $handle i32)
      (local $value i32)
      (local.set $handle (call $ctor))
      (local.set $value (call $value (local.get $handle)))
      (call $drop (local.get $handle))
      (local.get $value)
    )
  )
  (core instance $i (instantiate $m (with "env" (instance $env))))
  (alias core export $i "run" (core func $run))
  (func (export "run") (result u32) (canon lift (core func $run)))
)
