;; Item 4, probe 9: a GC array. Accepted or refused, never silent
;; (docs/TESTING.md section 6).
(module
  (type $row (array (mut i32)))
  (func (export "make") (result (ref $row))
    i32.const 3
    i32.const 7
    array.new $row))
