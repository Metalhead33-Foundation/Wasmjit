;; A getter has no parameters (unless it is a method) and must return a value.
(component
  (import "[get]value" (func (param "x" u32) (result u32)))
)
