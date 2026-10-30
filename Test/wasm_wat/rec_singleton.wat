;; A single-member `rec` group wrapping a plain function type. It must intern to
;; the same identity as the equivalent bare type definition in rec_groups.wat.
(module
  (rec
    (type $f (func (param i32) (result i32)))))
