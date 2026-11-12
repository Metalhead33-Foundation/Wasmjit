;; A narrow module that imports the *wide* export of call_form_provider.wat.
;; Under the per-signature rule the provider compiles `wide` in the frame form
;; and `narrow` in the register form; this module emits the *callee's* form for
;; the import, so the two forms meet correctly instead of the link being refused
;; (GAP-1, GR-8).
(module
  (import "provider" "wide" (func $wide (param i32 i32 i32 i32 i32 i32) (result i32)))
  (func (export "call_wide") (result i32)
    i32.const 40
    i32.const 2
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    call $wide))
