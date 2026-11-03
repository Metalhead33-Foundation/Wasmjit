;; Resource destructor ownership across an instance boundary: the dropped
;; handle is in `target`, but its destructor must run in the runtime that
;; created the resource.
(component
  (core module $m
    (global $drops (export "drops") (mut i32) (i32.const 0))
    (func (export "dtor") (param i32)
      (global.set $drops (i32.add (global.get $drops) (i32.const 1)))))
  (core instance $i (instantiate $m))
  (alias core export $i "dtor" (core func $dtor))
  (type $R (resource (rep i32) (dtor (func $dtor))))
  (canon resource.new $R (core func $new))
  (canon resource.drop $R (core func $drop))
  (alias core export $i "drops" (core global $drops))
)
