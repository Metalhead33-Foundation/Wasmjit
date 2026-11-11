;; Item 4, probe 14: array operations beyond construction -- array.set and array.fill.
;;
;; The first version of this fixture only *executed* the stores and returned a
;; constant, so it could not tell a working lowering from one that traps or does
;; nothing: it measured the module's instantiation, not the feature. Each export
;; here now returns the value it read back, so the assertion is the round-trip
;; (docs/TESTING.md section 8).
(module
  (type $row (array (mut i32)))

  ;; array.set, then read the element back.
  (func (export "set_get") (result i32) (local $a (ref $row))
    (local.set $a (array.new $row (i32.const 5) (i32.const 2)))
    (array.set $row (local.get $a) (i32.const 0) (i32.const 9))
    (array.get $row (local.get $a) (i32.const 0)))

  ;; array.fill, then read an element the fill covered.
  (func (export "fill_get") (result i32) (local $a (ref $row))
    (local.set $a (array.new $row (i32.const 5) (i32.const 2)))
    (array.fill $row (local.get $a) (i32.const 0) (i32.const 5) (i32.const 2))
    (array.get $row (local.get $a) (i32.const 1)))

  ;; The original one-expression form, kept because it exercises the operand
  ;; stack directly (no local between the constructor and the store).
  (func (export "inline") (result i32)
    i32.const 5
    i32.const 2
    array.new $row
    i32.const 0
    i32.const 9
    array.set $row
    i32.const 5
    i32.const 2
    array.new $row
    i32.const 0
    i32.const 5
    i32.const 2
    array.fill $row
    i32.const 0))
