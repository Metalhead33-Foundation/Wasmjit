;; Validator fixture for ref.test/ref.cast (docs/proposals/GC.md G9/GC-10).
;; The operand only has to be a reference in the target's hierarchy; its
;; nullability is irrelevant. Both target-nullability forms are covered.
(module
  (type $base (sub (struct (field i32))))
  (type $derived (sub $base (struct (field i32) (field i64))))

  (func (export "test_any") (param anyref) (result i32)
    local.get 0 ref.test (ref $base))
  (func (export "test_nullable") (param (ref null $base)) (result i32)
    local.get 0 ref.test (ref null $derived))
  (func (export "cast_any") (param anyref) (result (ref $base))
    local.get 0 ref.cast (ref $base))
  (func (export "cast_null") (param anyref) (result (ref null $base))
    local.get 0 ref.cast (ref null $base)))
