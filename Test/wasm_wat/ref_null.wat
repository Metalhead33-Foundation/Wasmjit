;; The first reference slice: ref.null, ref.is_null and ref.eq. Every value in
;; this fixture is null. `ref.eq` is lowered but not fixtured: it requires two
;; `eqref` operands, and constructing one needs `ref.func` (see the post-mortem).
(module
  (func (export "null_is_null") (result i32)
    ref.null func
    ref.is_null)
  (func (export "null_extern_is_null") (result i32)
    ref.null extern
    ref.is_null)
)
