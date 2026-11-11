;; GAP-11: a wide vector signature behind an indirect call, in a module that also
;; imports a native function. The staged-import adapter used to have no ffi_type
;; for a v128 and refused the whole module; the call site now carries the vector
;; through the staging frame (docs/API_ABI_STABILITY.md 8.5, `GAP-11`).
;;
;; The callee of the indirect call is a compiled function, so this exercises the
;; call site's runtime form branch and the frame layout of a wide vector
;; signature; `wide_v128_import_probe.wat` covers the closure itself.
(module
  (import "env" "host_add" (func $host_add (param i32) (result i32)))
  (type $wide (func (param v128 i32 i32 i32) (result i32)))
  (table 1 funcref)
  (elem (i32.const 0) $lane)

  (func $lane (param v128 i32 i32 i32) (result i32)
    local.get 0
    i32x4.extract_lane 2)

  (func (export "indirect") (result i32)
    v128.const i32x4 10 20 30 40
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    call_indirect (type $wide))

  (func (export "host") (param i32) (result i32)
    local.get 0
    call $host_add))
