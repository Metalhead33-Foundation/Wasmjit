;; A resource defined inside an instantiated component is generative: two
;; instantiations of the same definition must not share its identity.
(component
  (component $maker
    (type $r (resource (rep i32)))
    (export "r" (type $r))
  )
  (instance $a (instantiate $maker))
  (instance $b (instantiate $maker))
)
