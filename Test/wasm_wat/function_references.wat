;; docs/proposals/FUNCTION_REFERENCES.md FR-6/FR-8: typed function references
;; survive in a typed table and a typed mutable global, and call_ref /
;; return_call_ref consume them.
(module
  (type $f (func (param i32) (result i32)))
  (table $t 1 (ref null $f))
  (global $g (mut (ref null $f)) (ref.null $f))
  (elem declare func $inc)
  (func $inc (type $f) (param i32) (result i32)
    (i32.add (local.get 0) (i32.const 1)))
  (func (export "store")
    (table.set $t (i32.const 0) (ref.func $inc))
    (global.set $g (ref.func $inc)))
  (func (export "is-null") (result i32)
    (i32.or
      (ref.is_null (table.get $t (i32.const 0)))
      (ref.is_null (global.get $g))))
  (func (export "call-table") (param i32) (result i32)
    (call_ref $f (local.get 0) (table.get $t (i32.const 0))))
  (func (export "call-global") (param i32) (result i32)
    (call_ref $f (local.get 0) (global.get $g)))
  (func (export "tail") (param i32) (result i32)
    (return_call_ref $f (local.get 0) (ref.func $inc))))
