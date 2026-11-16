;; A component that imports one resource-free WASI 0.2 interface and calls it:
;; `wasi:clocks/monotonic-clock.now` is lowered into the core module, which
;; returns `now() + 1` and lifts it. This is the end-to-end host-import fixture
;; the hermetic stub satisfies.
(component
  (import "wasi:clocks/monotonic-clock@0.2.0" (instance $clock
    (export "now" (func $now (result u64)))
  ))
  (alias export $clock "now" (func $now-imported))
  (canon lower (func $now-imported) (core func $now-core))
  (core instance $env (export "now" (func $now-core)))
  (core module $m
    (import "env" "now" (func $now (result i64)))
    (func (export "get") (result i64)
      (i64.add (call $now) (i64.const 1)))
  )
  (core instance $i (instantiate $m (with "env" (instance $env))))
  (alias core export $i "get" (core func $get))
  (func (export "get") (result u64) (canon lift (core func $get)))
)
