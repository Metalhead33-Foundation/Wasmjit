;; GAP-23: an imported interface that exports a **resource type**, with one
;; method lowered out of it. The method's `borrow<res>` parameter is an imported
;; abstract resource, so its value is still a parser-local type index by the time
;; the Canonical ABI signature is asked for, and `canon lower` refuses the type.
;; A host WASI stub of `wasi:io/*` cannot be attached until this works.
(component
  (import "dep" (instance $dep
    (export "res" (type $res (sub resource)))
    (export "[method]res.f" (func $f (param "self" (borrow $res)) (result u32)))
  ))
  (alias export $dep "res" (type $res-alias))
  (alias export $dep "[method]res.f" (func $method))
  (canon lower (func $method) (core func $lowered))
)
