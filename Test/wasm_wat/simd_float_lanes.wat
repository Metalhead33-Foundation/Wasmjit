;; The seventh shared v128 slice: float lane access. These are the first ops
;; whose helpers take a float argument or return one, so the fixture observes
;; both a float result and its raw bits.
(module
  (func (export "f32x4_extract") (result f32)
    v128.const f32x4 1.5 2.5 3.5 4.5
    f32x4.extract_lane 2)
  (func (export "f64x2_extract") (result f64)
    v128.const f64x2 1.5 2.5
    f64x2.extract_lane 1)
  (func (export "f32x4_extract_negzero_bits") (result i32)
    v128.const f32x4 -0 0 0 0
    i32x4.extract_lane 0)
  (func (export "f32x4_splat") (param f32) (result f32)
    local.get 0
    f32x4.splat
    f32x4.extract_lane 3)
  (func (export "f64x2_splat") (param f64) (result f64)
    local.get 0
    f64x2.splat
    f64x2.extract_lane 1)
  (func (export "f32x4_splat_bits") (param f32) (result i32)
    local.get 0
    f32x4.splat
    i32x4.extract_lane 2)
  (func (export "f32x4_replace") (param f32) (result f32)
    v128.const f32x4 1 2 3 4
    local.get 0
    f32x4.replace_lane 2
    f32x4.extract_lane 2)
  (func (export "f32x4_replace_neighbour") (param f32) (result f32)
    v128.const f32x4 1 2 3 4
    local.get 0
    f32x4.replace_lane 2
    f32x4.extract_lane 1)
  (func (export "f64x2_replace") (param f64) (result f64)
    v128.const f64x2 1 2
    local.get 0
    f64x2.replace_lane 1
    f64x2.extract_lane 1)
  (func (export "f64x2_replace_low") (param f64) (result f64)
    v128.const f64x2 1 2
    local.get 0
    f64x2.replace_lane 0
    f64x2.extract_lane 1)
)
