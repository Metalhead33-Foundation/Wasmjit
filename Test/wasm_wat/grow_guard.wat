;; Unbounded memory and table, plus grows. This exists to prove the allocation
;; guardrail (docs/TESTING.md, WasmStore::allocationBudgetBytes): growing an
;; unbounded memory/table by 0xffffffff entries must fail as -1 *without*
;; attempting a ~256 TiB allocation. Before the guard this module could take the
;; whole host down, so the test doubles as the regression test for that.
(module
  (memory 1)           ;; no maximum: "unbounded"
  (table 1 funcref)    ;; no maximum

  (func (export "grow_memory") (param i32) (result i32)
    local.get 0
    memory.grow)

  (func (export "grow_table") (param i32) (result i32)
    ref.null func
    local.get 0
    table.grow)

  (func (export "table_size") (result i32)
    table.size))
