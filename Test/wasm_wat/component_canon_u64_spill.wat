;; GAP-20's counterpart. Two `u64` results really are two flat core values, so
;; MAX_FLAT_RESULTS = 1 is exceeded and the lift must carry a `memory` option.
;; The fixture keeps the rule honest after the 64-bit counting fix: the limit
;; still bites, it is only counted in core values.
(component
  (core module $m
    (func (export "pair") (result i64 i64) (i64.const 1) (i64.const 2)))
  (core instance $i (instantiate $m))
  (alias core export $i "pair" (core func $f))
  (func (export "pair") (result (tuple u64 u64)) (canon lift (core func $f)))
)
