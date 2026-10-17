;; Core struct and array type definitions inside a component.
(component
  (core type $s (struct (field i32) (field (mut i64))))
  (core type $a (array (mut i8)))
)