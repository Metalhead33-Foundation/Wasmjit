;; CM17: a host-supplied component instance import. The component aliases the
;; function out of the imported bag and re-exports it; the runtime must wrap the
;; ComponentInstanceValue in a component-instance handle.
(component
  (import "api" (instance $api
    (export "f" (func (param "x" u32) (result u32)))))
  (alias export $api "f" (func $f))
  (export "f" (func $f))
)
