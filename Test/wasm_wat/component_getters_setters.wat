;; Getters/setters: a plain property, a method property and a static property.
;; Each setter is preceded by its matching getter and repeats its property type.
(component
  (import "value" (type $V (sub resource)))
  (import "[get]count" (func (result u32)))
  (import "[set]count" (func (param "value" u32)))
  (import "[get]name" (func (result string)))
  (import "[set]name" (func (param "value" string)))

  (import "R" (type $R (sub resource)))
  (import "[method][get]R.field" (func (param "self" (borrow $R)) (result u32)))
  (import "[method][set]R.field" (func (param "self" (borrow $R)) (param "value" u32)))
  (import "[static][get]R.stat" (func (result u32)))
  (import "[static][set]R.stat" (func (param "value" u32)))
)
