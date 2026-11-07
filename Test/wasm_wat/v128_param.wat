;; A v128 parameter crosses the host boundary as a 16-byte integer pair, i.e.
;; two argument slots. The second export puts a scalar first so a slot-numbering
;; mistake shows up.
(module
  (func (export "lane1") (param v128) (result i32)
    local.get 0
    i32x4.extract_lane 1)
  (func (export "lane3_after_scalar") (param i32 v128) (result i32)
    local.get 1
    i32x4.extract_lane 3)
)
