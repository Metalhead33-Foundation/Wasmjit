;; GAP-22: core instantiation matches an imported memory by *subtyping*, not by
;; equality. The importing module asks for `(memory 0)` (min 0, no maximum) and
;; the argument exports `(memory 1)`; a provided minimum that is larger than the
;; declared one is a valid import match, which is how every preview1 adapter
;; imports the memory it is handed.
(component
  (core module $m
    (memory (export "mem") 1))
  (core instance $i (instantiate $m))
  (core module $user
    (import "env" "mem" (memory 0)))
  (core instance $u (instantiate $user (with "env" (instance $i))))
)
