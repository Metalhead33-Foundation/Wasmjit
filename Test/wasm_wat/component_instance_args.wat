;; CM17: instantiate arguments of every runtime sort. The child takes a value
;; (host import), an instance (a bag holding a lifted function) and a type (a
;; resource), then re-exports the function and the value. Before CM17 only
;; function arguments were accepted and every other sort threw.
(component
  (core module $m
    (func (export "f") (param i32) (result i32)
      (local.get 0)
    )
  )
  (core instance $ci (instantiate $m))
  (alias core export $ci "f" (core func $cf))
  (type $ft (func (param "x" u32) (result u32)))
  (canon lift (core func $cf) (func $f (type $ft)))
  (instance $api (export "f" (func $f)))
  (type $res (resource (rep i32)))
  (import "hostv" (value $hv u32))
  (component $child
    (import "v" (value $v u32))
    (import "api" (instance $api (export "f" (func (param "x" u32) (result u32)))))
    (import "t" (type $t (sub resource)))
    (alias export $api "f" (func $call))
    (export "call" (func $call))
    (export "out" (value $v))
  )
  (instance $childInstance (instantiate $child
      (with "v" (value $hv))
      (with "api" (instance $api))
      (with "t" (type $res))))
  (alias export $childInstance "call" (func $call2))
  (alias export $childInstance "out" (value $out))
  (export "call" (func $call2))
  (export "out" (value $out))
)
