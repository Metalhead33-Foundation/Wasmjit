;; Exercises core type definitions inside a component, including a core
;; module type with an import, an export and a type declaration.
(component
  (core type $ft (func (param i32) (result i32)))
  (core type $mt (module
    (import "m" "x" (func (param i32)))
    (export "y" (func (result i32)))
    (type $inner (func (param i64)))
  ))
)