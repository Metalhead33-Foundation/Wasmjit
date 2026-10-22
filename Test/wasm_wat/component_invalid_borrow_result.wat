;; borrow is not allowed in a function result
(component
  (type $r (resource (rep i32)))
  (type $f (func (result (borrow $r))))
)