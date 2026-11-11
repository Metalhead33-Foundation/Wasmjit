;; A v128 result, on its own fixture: the canonical rule says it crosses through a
;; caller-supplied 16-byte buffer, and this pins what the AsmJIT backend does today
;; (docs/backends/HISTORY.md, follow-up 1). Kept out of the shared
;; fixture so no other backend has to compile the shape
;; (docs/backends/HISTORY.md).
(module
  (func (export "vec_result") (result v128)
    v128.const i32x4 7 8 9 10))
