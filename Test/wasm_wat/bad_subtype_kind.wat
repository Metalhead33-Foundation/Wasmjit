;; Invalid `sub`: a struct cannot be a subtype of a function type.
(module
  (type $f (sub (func)))
  (type $d (sub $f (struct (field i32)))))
