;; docs/proposals/PROFILES.md PR-4: a declared v128 value type is rejected by the
;; scalar profile at the type scan (no SIMD instruction is needed).
(module
  (func (export "keep") (param v128) (result v128)
    local.get 0))
