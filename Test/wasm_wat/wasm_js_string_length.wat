;; docs/proposals/JS_INTEGRATION.md JS-0/JS-1: a JavaScript-shaped import name is
;; just an ordinary (module, field) pair. The core reserves nothing and links no
;; JavaScript engine; an embedder adapter may supply this callable.
(module
  (import "wasm:js-string" "length"
    (func $length (param externref) (result i32)))
  (func (export "len") (param externref) (result i32)
    local.get 0
    call $length)
  ;; Opaque `externref` round-trip with no JavaScript object model involved.
  (func (export "identity") (param externref) (result externref)
    local.get 0))
