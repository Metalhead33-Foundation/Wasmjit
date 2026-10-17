;; a type index used as a func type must actually be a function type
(component
  (type $t (instance))
  (import "f" (func (type $t)))
)