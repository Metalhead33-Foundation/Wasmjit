;; Core module type with every core external kind in its import list.
(component
  (core type $mt (module
    (import "m" "t" (table 1 funcref))
    (import "m" "mem" (memory 1))
    (import "m" "g" (global i32))
    (import "m" "tag" (tag (param i32)))
  ))
)