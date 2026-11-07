;; The eighth shared v128 slice: the transport families — extending,
;; splatting and zeroing loads, lane loads and lane stores, and swizzle.
;; Results are observed through i32x4 lanes.
(module
  (memory 1)
  (func (export "load8x8_s") (result i32)
  i32.const 0
  i32.const -1
  i32.store
  i32.const 0
  v128.load8x8_s
  i32x4.extract_lane 0)
  (func (export "load8x8_u") (result i32)
  i32.const 0
  i32.const -1
  i32.store
  i32.const 0
  v128.load8x8_u
  i32x4.extract_lane 0)
  (func (export "load16x4_s") (result i32)
  i32.const 0
  i32.const -1
  i32.store
  i32.const 0
  v128.load16x4_s
  i32x4.extract_lane 0)
  (func (export "load16x4_u") (result i32)
  i32.const 0
  i32.const -1
  i32.store
  i32.const 0
  v128.load16x4_u
  i32x4.extract_lane 0)
  (func (export "load32x2_s") (result i32)
  i32.const 0
  i32.const -1
  i32.store
  i32.const 0
  v128.load32x2_s
  i32x4.extract_lane 0)
  (func (export "load32x2_u") (result i32)
  i32.const 0
  i32.const 65535
  i32.store
  i32.const 0
  v128.load32x2_u
  i32x4.extract_lane 0)
  (func (export "load8_splat") (result i32)
  i32.const 0
  i32.const 16843009
  i32.store
  i32.const 0
  v128.load8_splat
  i32x4.extract_lane 0)
  (func (export "load16_splat") (result i32)
  i32.const 0
  i32.const 2
  i32.store
  i32.const 0
  v128.load16_splat
  i32x4.extract_lane 0)
  (func (export "load32_splat") (result i32)
  i32.const 0
  i32.const 5
  i32.store
  i32.const 0
  v128.load32_splat
  i32x4.extract_lane 0)
  (func (export "load64_splat") (result i32)
  i32.const 0
  i64.const 7
  i64.store
  i32.const 0
  v128.load64_splat
  i32x4.extract_lane 0)
  (func (export "load64_splat_hi") (result i32)
  i32.const 0
  i64.const 7
  i64.store
  i32.const 0
  v128.load64_splat
  i32x4.extract_lane 1)
  (func (export "load32_zero") (result i32)
  i32.const 0
  i32.const 5
  i32.store
  i32.const 0
  v128.load32_zero
  i32x4.extract_lane 0)
  (func (export "load32_zero_hi") (result i32)
  i32.const 0
  i32.const 5
  i32.store
  i32.const 0
  v128.load32_zero
  i32x4.extract_lane 1)
  (func (export "load64_zero") (result i32)
  i32.const 0
  i64.const 7
  i64.store
  i32.const 0
  v128.load64_zero
  i32x4.extract_lane 0)
  (func (export "load64_zero_hi") (result i32)
  i32.const 0
  i64.const 7
  i64.store
  i32.const 0
  v128.load64_zero
  i32x4.extract_lane 2)
  (func (export "load8_lane") (result i32)
  i32.const 1
  i32.const 66
  i32.store8
  i32.const 1
  v128.const i32x4 0 0 0 0
  v128.load8_lane 5
  i32x4.extract_lane 1)
  (func (export "load16_lane") (result i32)
  i32.const 0
  i32.const 4660
  i32.store16
  i32.const 0
  v128.const i32x4 0 0 0 0
  v128.load16_lane 0
  i32x4.extract_lane 0)
  (func (export "load32_lane") (result i32)
  i32.const 0
  i32.const -559038737
  i32.store
  i32.const 0
  v128.const i32x4 0 0 0 0
  v128.load32_lane 1
  i32x4.extract_lane 1)
  (func (export "load64_lane") (result i32)
  i32.const 0
  i64.const 1234605616436508552
  i64.store
  i32.const 0
  v128.const i32x4 0 0 0 0
  v128.load64_lane 0
  i32x4.extract_lane 1)
  (func (export "store8_lane") (result i32)
  i32.const 0
  v128.const i32x4 0x0000AB00 0 0 0
  v128.store8_lane 1
  i32.const 0
  i32.load8_u)
  (func (export "store16_lane") (result i32)
  i32.const 0
  v128.const i32x4 0x0000BEEF 0 0 0
  v128.store16_lane 0
  i32.const 0
  i32.load16_u)
  (func (export "store32_lane") (result i32)
  i32.const 0
  v128.const i32x4 5 0xCAFEBABE 0 0
  v128.store32_lane 1
  i32.const 0
  i32.load)
  (func (export "store64_lane") (result i32)
  i32.const 0
  v128.const i64x2 0x1122334455667788 0
  v128.store64_lane 0
  i32.const 0
  i32.load offset=4)
  (func (export "swizzle") (result i32)
  v128.const i32x4 0x04030201 0 0 0
  v128.const i32x4 0 0 0 0
  i8x16.swizzle
  i32x4.extract_lane 0)
  (func (export "swizzle_oob") (result i32)
  v128.const i32x4 0x04030201 0 0 0
  v128.const i32x4 0x10101010 0x10101010 0x10101010 0x10101010
  i8x16.swizzle
  i32x4.extract_lane 0)
)
