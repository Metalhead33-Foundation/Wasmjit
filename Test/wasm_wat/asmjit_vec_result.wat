;; A v128 result, on its own fixture: the canonical rule says it crosses through a
;; caller-supplied 16-byte buffer, and this pins what the AsmJIT backend does today
;; (docs/ABI_REFACTOR_POST_MORTEM.md section 6, follow-up 1). Kept out of the shared
;; fixture so no other backend has to compile the shape
;; (docs/ABI_REFACTOR_STUDY.md section 35).
(module
  (func (export "vec_result") (result v128)
    v128.const i32x4 7 8 9 10))
