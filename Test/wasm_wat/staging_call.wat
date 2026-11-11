;; A frame-form module with an INTERNAL call: one export takes three vectors and
;; calls a callee that also takes three, so both argument packing and result
;; unpacking cross the staging frame
;; (docs/ABI.md, step 3b). Own fixture, so no other backend
;; has to compile this shape (docs/backends/HISTORY.md).
(module
  (func $wide (param v128 v128 v128) (result i32)
    local.get 0
    i32x4.extract_lane 0
    local.get 1
    i32x4.extract_lane 0
    i32.add
    local.get 2
    i32x4.extract_lane 0
    i32.add)
  (func (export "via_call") (param v128 v128 v128) (result i32)
    local.get 0
    local.get 1
    local.get 2
    call $wide))
