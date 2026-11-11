;; Item 4, probe 17: array.new_fixed + array.get. Deliberately avoids array.new's
;; (value, length) order and both D-2 operations, so any failure is about array.get
;; (docs/TESTING.md section 11).
(module
  (type $row (array (mut i32)))
  (func (export "probe") (result i32)
    i32.const 11
    i32.const 22
    array.new_fixed $row 2
    i32.const 1
    array.get $row))
