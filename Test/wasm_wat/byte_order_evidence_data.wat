;; An active data segment copies four bytes into memory at instantiation, so those
;; bytes come from the binary in little-endian order and no policy may skip the
;; conversion (that is the whole reason `Adaptive` needs evidence).
(module
  (memory 1)
  (data (i32.const 0) "\01\00\00\00")
  (func (export "first") (result i32) (i32.load (i32.const 0)))
)
