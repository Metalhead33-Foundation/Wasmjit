;; The `[config]` fixture: one 4 MiB memory (64 pages of 64 KiB) and a trivial
;; export, so a runtime whose budget is below 4 MiB refuses to instantiate it and
;; a runtime with the default accepts it (docs/proposals/CONFIGURATION.md CFG-1).
(module
  (memory (export "memory") 64)
  (func (export "answer") (result i32)
    i32.const 42))
