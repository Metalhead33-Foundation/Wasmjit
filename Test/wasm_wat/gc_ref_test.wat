;; `ref.test` against a concrete heap type: the object header's type id is the
;; only thing consulted, and null is handled by the nullable form.
(module
  (type $a (struct (field i32)))
  ;; Structurally distinct from $a on purpose: identical definitions are
  ;; canonicalised to one type id, so they could not test a mismatch.
  (type $b (struct (field i64)))
  (func (export "test_same") (result i32)
    (ref.test (ref $a) (struct.new $a (i32.const 1))))
  (func (export "test_other") (result i32)
    (ref.test (ref $b) (struct.new $a (i32.const 1))))
  (func (export "test_null") (result i32)
    (ref.test (ref $a) (ref.null $a)))
  (func (export "test_null_ok") (result i32)
    (ref.test (ref null $a) (ref.null $a)))
)
