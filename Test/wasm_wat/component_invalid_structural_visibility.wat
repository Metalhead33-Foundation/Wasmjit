;; A local record has no external name, so an import may not expose it.
(component
  (type $r (record (field "x" u32)))
  (type $f (func (param "x" $r)))
  (import "f" (func $f (type $f)))
)
