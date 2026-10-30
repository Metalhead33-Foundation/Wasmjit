;; ENS7 / docs/proposals/EXTENDED_NAME_SECTION.md: the access traps *inside* a
;; native helper (wasm_atomic.cpp has no compile-time site), so the generated
;; code has to annotate the pending trap with the calling function.
(module
  (memory 1 1)
  (func (export "misaligned") (result i32)
    (i32.atomic.load (i32.const 1))))
