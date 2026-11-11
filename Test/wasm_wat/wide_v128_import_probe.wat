;; `GAP-11`, second half: a frame-form module routes every native import through
;; the staged libffi closure, so a host function that takes a `v128` parameter is
;; the shape that makes the closure itself carry a vector. The canonical spelling
;; is two `uint64_t` arguments, low word first (docs/ABI.md "The C ABI Contract").
;;
;; `$wideUser` is what selects the frame form: a *defined* function whose C
;; argument entries exceed SLJIT's ceiling (one vector is two words, so a vector
;; plus two scalars is five entries against a ceiling of four). A register-form
;; module never needs the adapter for a three-parameter import, so the closure is
;; unreachable without it.
(module
  (import "env" "lane_of" (func $lane_of (param v128 i32) (result i32)))

  (func $wideUser (param v128 i32 i32) (result i32)
    local.get 0
    local.get 1
    call $lane_of)

  (func (export "staged") (param i32) (result i32)
    v128.const i32x4 10 20 30 40
    i32.const 2
    i32.const 0
    call $wideUser))
