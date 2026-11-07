;; Bulk memory on a memory64 is a documented P2 residual refusal. The two
;; backends report it differently: SLJIT leaves this function without an entry
;; point and keeps the module usable, while AsmJit fails the whole module
;; transactionally (its long-standing refusal model).
(module
  (memory i64 1)
  (func (export "copy") (param i64 i64 i64)
    local.get 0
    local.get 1
    local.get 2
    memory.copy)
)
