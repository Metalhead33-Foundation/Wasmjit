;; One rec group with a subtype relation: $derived is `(sub $base ...)`.
;; Exercises supertype canonicalization (an intra-group `Rec` reference), the
;; derived `depth`, and the fact that finality/supertypes affect identity.
(module
  (rec
    (type $base (sub (struct (field i32))))
    (type $derived (sub $base (struct (field i32))))))
