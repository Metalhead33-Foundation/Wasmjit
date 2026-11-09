;; Bulk memory on a memory64 is lowered by all three backends; the fixture is a
;; zero-length copy used by the AsmJIT and SLJIT regression tests.
(module
  (memory i64 1)
  (func (export "copy") (param i64 i64 i64)
    local.get 0
    local.get 1
    local.get 2
    memory.copy)
)
