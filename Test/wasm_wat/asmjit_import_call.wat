;; A frame-mode module that calls an IMPORT: the refusal P0b guards
;; (docs/ABI.md). Own fixture so no other backend
;; has to compile the shape.
(module
  (import "env" "host_fn" (func $host (param i32) (result i32)))
  (func (export "caller") (result i32)
    i32.const 7
    call $host))
