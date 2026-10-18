;; Wide arithmetic fixture (docs/proposals/NUMERIC_PROPOSALS.md NUM-1).
;; Operand order is lhs_lo lhs_hi rhs_lo rhs_hi; results are lo then hi.
(module
  (func (export "add128") (param i64 i64 i64 i64) (result i64 i64)
    local.get 0 local.get 1 local.get 2 local.get 3 i64.add128)
  (func (export "sub128") (param i64 i64 i64 i64) (result i64 i64)
    local.get 0 local.get 1 local.get 2 local.get 3 i64.sub128)
  (func (export "mul_wide_s") (param i64 i64) (result i64 i64)
    local.get 0 local.get 1 i64.mul_wide_s)
  (func (export "mul_wide_u") (param i64 i64) (result i64 i64)
    local.get 0 local.get 1 i64.mul_wide_u))
