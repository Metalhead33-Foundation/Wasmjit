;; Item 4, probe 8: a memory64 module. Accepted or refused, never silent
;; (docs/TESTING.md section 5).
(module
  (memory i64 1)
  (func (export "pages") (result i64)
    memory.size))
