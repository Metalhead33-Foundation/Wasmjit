;; Outer alias inside an instance type.
(component
  (core type $t (func))
  (type $it (instance
    (alias outer 0 0 (core type $a))
  ))
)