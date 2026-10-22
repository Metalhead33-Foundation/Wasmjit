;; An exported core module introduces a core-module index that can be
;; instantiated later.
(component
  (core module $m (func (export "f")))
  (export "m" (core module $m))
  (core instance (instantiate 1))
)