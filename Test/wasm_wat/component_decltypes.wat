;; Component and instance type declarators, including nested imports/exports.
(component
  (type $ct (component
    (import "x" (func (param "a" u32)))
    (export "y" (func))
  ))
  (type $it (instance
    (export "z" (func))
  ))
)