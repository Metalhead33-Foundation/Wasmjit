;; Item 4, probe 15: funcref in a table plus call_indirect
;; (docs/backends/CAPABILITY_PROBES.md section 9).
(module
  (type $bin (func (param i32 i32) (result i32)))
  (func $add (type $bin) (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add)
  (table 1 funcref)
  (elem (i32.const 0) $add)
  (func (export "via_table") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.const 0
    call_indirect (type $bin)))
