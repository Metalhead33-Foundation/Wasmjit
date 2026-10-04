;; Invalid `sub`: a bare type definition is final, and a final type may not be
;; used as a supertype.
(module
  (type $base (struct (field i32)))
  (type $derived (sub $base (struct (field i32)))))
