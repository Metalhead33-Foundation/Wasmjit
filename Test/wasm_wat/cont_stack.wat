;; A-provider stack guard: the continuation recurses until the generated
;; prologue's `wasm_stack_check` reports CallStackExhausted. The host sees a
;; trap, not a host SIGSEGV.
(module
  (type $ft (func (result i32)))
  (type $ct (cont $ft))

  (func $gen (type $ft)
    call $gen)

  (elem declare func $gen)

  (func (export "make") (result (ref $ct))
    ref.func $gen
    cont.new $ct))
