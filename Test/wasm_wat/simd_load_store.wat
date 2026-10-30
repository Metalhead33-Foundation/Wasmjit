;; SIMD-L2: memory ops end to end through the JIT (docs/SIMD_LIBJIT.md §10).
;;
;; The harness cannot marshal a `v128` yet (gap S7), so every export returns a
;; scalar projection of the result. The data segment at offset 0 is the 16 bytes
;; 01..10, which makes the lane arithmetic checkable by hand.
(module
  (memory (export "mem") 1)
  (data (i32.const 0) "\01\02\03\04\05\06\07\08\09\0a\0b\0c\0d\0e\0f\10")

  ;; v128.load: lane 0 is bytes 01 02 03 04 little-endian.
  (func (export "load_lane0") (result i32)
    (i32x4.extract_lane 0 (v128.load (i32.const 0))))
  (func (export "load_lane3") (result i32)
    (i32x4.extract_lane 3 (v128.load (i32.const 0))))

  ;; An unaligned load: the memarg alignment is smaller than natural, which is
  ;; legal and must behave exactly like an aligned access.
  (func (export "load_unaligned_lane0") (result i32)
    (i32x4.extract_lane 0 (v128.load offset=1 align=1 (i32.const 0))))

  ;; v128.store: write a vector, read the bytes back with scalar loads.
  (func (export "store_roundtrip") (result i32)
    (v128.store (i32.const 64) (v128.const i32x4 0x11223344 0 0 0))
    (i32.load (i32.const 64)))
  (func (export "store_roundtrip2") (result i32)
    (v128.store (i32.const 80) (v128.const i32x4 0x11223344 0x55667788 0 0))
    (i32.load offset=4 (i32.const 80)))

  ;; The widening loads: 8 bytes in, sign- or zero-extended to 16.
  (func (export "load8x8_s_lane0") (result i32)
    (i16x8.extract_lane_s 0 (v128.load8x8_s (i32.const 0))))
  (func (export "load8x8_s_lane7") (result i32)
    (i16x8.extract_lane_s 7 (v128.load8x8_s (i32.const 0))))
  (func (export "load8x8_u_lane7") (result i32)
    (i16x8.extract_lane_u 7 (v128.load8x8_u (i32.const 0))))
  (func (export "load16x4_s_lane1") (result i32)
    (i32x4.extract_lane 1 (v128.load16x4_s (i32.const 0))))

  ;; Broadcasts: every lane gets the scalar at the address.
  (func (export "load8_splat_lane3") (result i32)
    (i8x16.extract_lane_u 3 (v128.load8_splat (i32.const 1))))
  (func (export "load32_splat_lane2") (result i32)
    (i32x4.extract_lane 2 (v128.load32_splat (i32.const 0))))
  (func (export "load64_splat_lane1") (result i64)
    (i64x2.extract_lane 1 (v128.load64_splat (i32.const 0))))

  ;; Zero-extending loads: lane 0 is the value, every other lane is zero.
  (func (export "load32_zero_lane0") (result i32)
    (i32x4.extract_lane 0 (v128.load32_zero (i32.const 0))))
  (func (export "load32_zero_lane1") (result i32)
    (i32x4.extract_lane 1 (v128.load32_zero (i32.const 0))))
  (func (export "load64_zero_lane1") (result i64)
    (i64x2.extract_lane 1 (v128.load64_zero (i32.const 0))))

  ;; Lane load: only the named lane comes from memory.
  (func (export "load32_lane1") (result i32)
    (i32x4.extract_lane 1
      (v128.load32_lane 1 (i32.const 4) (v128.const i32x4 0 0 0 0))))
  (func (export "load16_lane0") (result i32)
    (i32x4.extract_lane 0
      (v128.load16_lane 0 (i32.const 0) (v128.const i32x4 0xFFFFFFFF 0 0 0))))

  ;; Lane store: only the named lane reaches memory.
  (func (export "store32_lane2") (result i32)
    (v128.store32_lane 2 (i32.const 100) (v128.const i32x4 0 0 0xAABBCCDD 0))
    (i32.load (i32.const 100)))
  (func (export "store8_lane0_neighbour") (result i32)
    ;; Byte 0 is 0x7F and byte 1 is 0xFF, so if the helper wrote the whole
    ;; vector to memory, address 121 would read 0xFF instead of 0.
    (v128.store8_lane 0 (i32.const 120) (v128.const i32x4 0x0000FF7F 0 0 0))
    (i32.load8_u (i32.const 121))))
