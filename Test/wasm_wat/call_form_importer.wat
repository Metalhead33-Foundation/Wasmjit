;; A register-form module that imports the *narrow* export of
;; call_form_provider.wat. Its own signatures all fit the register form, so it
;; calls in that form; the provider compiled the same function into whatever
;; form its wide sibling forced. The two modules therefore disagree, and the
;; engine must refuse the link instead of corrupting memory.
;;
;; Instantiated with `WASM::setFrameCallAbiOptIn(true)` both modules use the
;; frame form and the call works -- which is the other half of the fixture.
(module
  (import "provider" "narrow" (func $narrow (param i32) (result i32)))
  (func (export "call_narrow") (param i32) (result i32)
    (call $narrow (local.get 0)))
)
