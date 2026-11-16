;; GAP-22's counterpart: subtyping is not "anything goes". The importing module
;; asks for `(memory 2)` and the argument exports `(memory 1)`, whose minimum is
;; *smaller* than the declared one, so the instantiation must still be rejected.
(component
  (core module $m
    (memory (export "mem") 1))
  (core instance $i (instantiate $m))
  (core module $user
    (import "env" "mem" (memory 2)))
  (core instance $u (instantiate $user (with "env" (instance $i))))
)
