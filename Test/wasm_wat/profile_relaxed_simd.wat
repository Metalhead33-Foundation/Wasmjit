;; docs/proposals/PROFILES.md PR-2/PRD5: the `N` marker excludes relaxed SIMD,
;; whose result is allowed to differ between hosts.
(module
  (func (export "madd") (result v128)
    (f32x4.relaxed_madd
      (v128.const f32x4 1 2 3 4)
      (v128.const f32x4 1 2 3 4)
      (v128.const f32x4 1 2 3 4))))
