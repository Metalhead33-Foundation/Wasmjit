;; Invalid `sub`: the derived struct's field (i64) does not match the declared
;; supertype's field (i32). Must be rejected by declared-subtype validation.
(module
  (rec
    (type $base (sub (struct (field i32))))
    (type $derived (sub $base (struct (field i64))))))
