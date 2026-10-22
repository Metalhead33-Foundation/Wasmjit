;; `v128` globals (docs/proposals/SIMD.md SIMD-4 / SIMD-L5): the constant-expression
;; path, `global.get`, `global.set`, and a mutable global carrying state across
;; calls. Nothing here is exotic — that is the point: `v128` globals are the last
;; SIMD feature that was missing, and each of these shapes goes through a different
;; piece of the engine (evalConstantExpr, the load helper, the store helper).
(module
  (global $mut (mut v128) (v128.const i32x4 1 2 3 4))
  (global $imm v128 (v128.const i8x16 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15))

  ;; a constant global, read lane by lane
  (func (export "imm_lane2") (result i32)
    (i32x4.extract_lane 2 (global.get $imm)))

  ;; the `v128.const` byte order through the constant-expression path (hazard Y11)
  (func (export "imm_byte15") (result i32)
    (i8x16.extract_lane_u 15 (global.get $imm)))

  ;; the mutable global's initial value
  (func (export "mut_lane0") (result i32)
    (i32x4.extract_lane 0 (global.get $mut)))

  ;; `global.set` of a fresh constant
  (func (export "set_and_read") (result i32)
    (global.set $mut (v128.const i32x4 7 8 9 10))
    (i32x4.extract_lane 3 (global.get $mut)))

  ;; read-modify-write: the value must survive between calls, so this only gives 8
  ;; if the earlier `set_and_read` really stored 7 in lane 0
  (func (export "accumulate") (param v128) (result i32)
    (global.set $mut (i32x4.add (global.get $mut) (local.get 0)))
    (i32x4.extract_lane 0 (global.get $mut)))

  ;; a `v128` global copied through a `v128` local
  (func (export "copy_from_imm") (result i32)
    (local $v v128)
    (local.set $v (global.get $imm))
    (i32x4.extract_lane 3 (local.get $v)))
)
