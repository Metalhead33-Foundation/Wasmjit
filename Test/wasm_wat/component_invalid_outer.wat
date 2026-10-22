;; outer alias 0 refers to the instance type's own empty core-type space
(component
  (core type $t (func))
  (type $it (instance
    (alias outer 0 0 (core type $a))
  ))
)