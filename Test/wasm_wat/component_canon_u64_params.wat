;; GAP-20 regression, parameter side. Nine `u64` parameters are nine flat core
;; values (not eighteen 32-bit words), so MAX_FLAT_PARAMS = 16 still holds and
;; the bare `canon lift` needs no `memory` option.
(component
  (core module $m
    (func (export "sum")
      (param i64 i64 i64 i64 i64 i64 i64 i64 i64) (result i64)
      (local.get 0)))
  (core instance $i (instantiate $m))
  (alias core export $i "sum" (core func $f))
  (func (export "sum")
    (param "a" u64) (param "b" u64) (param "c" u64)
    (param "d" u64) (param "e" u64) (param "f" u64)
    (param "g" u64) (param "h" u64) (param "p" u64)
    (result u64)
    (canon lift (core func $f))))
