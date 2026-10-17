;; resource definitions are not allowed in component/instance type scopes
(component
  (type (instance
    (type $r (resource (rep i32)))
  ))
)