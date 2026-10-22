;; SIMD-L1: the `v128` calling convention and the local slot
;; (docs/SIMD_LIBJIT.md §10, milestone SIMD-L1; gaps S4/S9).
;;
;; The spec harness cannot marshal a `v128` yet (gap S7), so every export either
;; round-trips the value back to C++ or projects one lane to a scalar — which is
;; exactly enough to prove the JIT's lowered 16-byte struct, its 16-byte
;; alignment, and that a `v128` local is a real slot.
(module
  ;; The SIMD-0 "done when": a function that takes a `v128` and returns it.
  (func (export "identity") (param $v v128) (result v128)
    (local.get $v))

  ;; Through a local: the local's frame slot must be written and read back
  ;; byte-exactly.
  (func (export "via_local") (param $v v128) (result v128)
    (local $t v128)
    (local.set $t (local.get $v))
    (local.get $t))

  ;; A `v128` local starts *zeroed*, not garbage: this reads one before its
  ;; first `local.set`, so it exercises zeroValueForType's 16-byte case.
  (func (export "local_zero_lane0") (result i32)
    (local $t v128)
    (i32x4.extract_lane 0 (local.get $t)))
  (func (export "local_zero_lane3") (result i32)
    (local $t v128)
    (i32x4.extract_lane 3 (local.get $t)))

  ;; Lane projections, so C++ can see the bytes as scalars.
  (func (export "lane0") (param $v v128) (result i32)
    (i32x4.extract_lane 0 (local.get $v)))
  (func (export "lane3") (param $v v128) (result i32)
    (i32x4.extract_lane 3 (local.get $v)))
  (func (export "u8_lane0") (param $v v128) (result i32)
    (i8x16.extract_lane_u 0 (local.get $v)))
  (func (export "u8_lane15") (param $v v128) (result i32)
    (i8x16.extract_lane_u 15 (local.get $v)))
  (func (export "i64_lane1") (param $v v128) (result i64)
    (i64x2.extract_lane 1 (local.get $v)))

  ;; A parameter passed straight through a block boundary (the block carries it
  ;; on the model stack).
  (func (export "through_block") (param $v v128) (result v128)
    (block (result v128)
      (local.get $v))))
