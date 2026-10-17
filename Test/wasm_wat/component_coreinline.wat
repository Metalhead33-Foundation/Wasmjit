;; Core instance expression with inline exports.
(component
  (core module $m (func (export "f")))
  (core instance $i0 (instantiate $m))
  (alias core export $i0 "f" (core func $f))
  (core instance $i (export "f" (func $f)))
)