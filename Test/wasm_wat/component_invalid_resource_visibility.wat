;; A locally defined resource has no external name, so a function import may
;; not expose it.
(component
  (type $r (resource (rep i32)))
  (type $f (func (result (own $r))))
  (import "f" (func $f (type $f)))
)
