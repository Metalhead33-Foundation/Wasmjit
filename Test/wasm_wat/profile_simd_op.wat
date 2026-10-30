;; docs/proposals/PROFILES.md PR-4: a SIMD instruction is rejected by the scalar
;; profile even though no v128 value type is declared in the type section.
(module
  (func (export "lane") (result i32)
    (i32x4.extract_lane 0 (v128.const i32x4 1 2 3 4))))
