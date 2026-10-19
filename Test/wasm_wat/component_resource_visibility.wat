;; An imported resource type is externally visible, so a function import may
;; use it in its result type.
(component
  (import "r" (type $r (sub resource)))
  (type $f (func (result (own $r))))
  (import "f" (func $f (type $f)))
)
