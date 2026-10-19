;; Constructor and method names bind to a preceding resource import.
(component
  (import "R" (type $R (sub resource)))
  (import "[constructor]R" (func (result (own $R))))
  (import "[method]R.do" (func (param "self" (borrow $R))))
)
