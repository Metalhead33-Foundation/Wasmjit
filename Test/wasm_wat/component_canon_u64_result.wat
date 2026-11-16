;; GAP-20 regression. `flatten_type` maps `u64` to ONE `i64` core value, so
;; `flatten_functype`'s MAX_FLAT_RESULTS = 1 is satisfied by `func () -> u64`
;; and a bare `canon lift` (no `memory` option) must validate.
(component
  (core module $m
    (func (export "get") (result i64) (i64.const 42)))
  (core instance $i (instantiate $m))
  (alias core export $i "get" (core func $f))
  (func (export "get") (result u64) (canon lift (core func $f)))
)
