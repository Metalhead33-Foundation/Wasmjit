;; Chunk 5: a function reference to an *imported* function. Both backends have
;; a separate path for import indices (SLJIT resolves the callable, AsmJit uses
;; the already-resolved import pointer), and neither was exercised until now.
(module
  (import "host" "f" (func $imp (result i32)))
  (elem declare func $imp)
  (func (export "import_ref_not_null") (result i32)
    ref.func $imp
    ref.is_null)
  (func (export "call_import") (result i32)
    call $imp)
)
