;; Outer alias inside an instance type (the enclosing component is two scopes
;; away from the type declarator's own scope).
(component
  (core type $t (func))
  (type $it (instance
    (alias outer 1 0 (core type $a))
  ))
)