;; Two recursive type groups: a 2-member `rec` group ($a/$b mutually refer to
;; each other), then a bare function type (a group of one). Used to check that
;; the parser preserves group boundaries and assigns distinct TypeIds.
(module
  (rec
    (type $a (struct (field (ref $b))))
    (type $b (struct (field (ref $a)))))
  (type $sig (func (param i32) (result i32))))
