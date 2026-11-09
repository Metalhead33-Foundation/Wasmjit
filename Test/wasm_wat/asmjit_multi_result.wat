;; Two results: refused today on every AsmJIT path, and the gap pin for multi-result
;; through the frame (docs/backends/EXPORT_SIGNATURE.md section 29).
(module
  (func (export "pair") (result i32 i64)
    i32.const 7
    i64.const 9))
