;; GAP-21: a `canon lower` of a function aliased out of an imported *instance*.
;; The function's resolved type never enters the component's local type index
;; space, so the validator has to intern that node on demand; before the fix it
;; reported that a plainly closed `func (result u64)` had "no closed identity"
;; and rejected a component `wasm-tools validate --features all` accepts.
(component
  (import "dep" (instance $dep
    (export "now" (func (result u64)))
  ))
  (alias export $dep "now" (func $now))
  (canon lower (func $now) (core func $cf))
)
