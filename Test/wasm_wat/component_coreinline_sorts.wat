;; Core instance inline bag carrying a table and a global. The runtime used to
;; refuse every sort but func and memory here ("inline core export sort is not
;; supported"), so this fixture is CM11's consumer test.
(component
  (core module $m
    (table (export "t") 1 funcref)
    (global (export "g") (mut i32) (i32.const 7))
  )
  (core instance $i0 (instantiate $m))
  (alias core export $i0 "t" (core table $t))
  (alias core export $i0 "g" (core global $g))
  (core instance $i (export "t" (table $t)) (export "g" (global $g)))
)
