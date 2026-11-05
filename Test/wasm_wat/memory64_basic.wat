;; The shared memory64 fixture for the AsmJit and SLJIT backends: the address
;; operand, memory.size/grow and the memarg offset are 64-bit, and the
;; 32-bit readers must not be mistaken for the value width.
(module
  (memory i64 1 2)
  (func (export "size") (result i64)
    memory.size)
  (func (export "grow") (param i64) (result i64)
    local.get 0
    memory.grow)
  (func (export "store_load_i32") (param i64 i32) (result i32)
    local.get 0
    local.get 1
    i32.store
    local.get 0
    i32.load)
  (func (export "store_load_i64") (param i64 i64) (result i64)
    local.get 0
    local.get 1
    i64.store
    local.get 0
    i64.load)
  (func (export "store_load_f32") (param i64 f32) (result f32)
    local.get 0
    local.get 1
    f32.store
    local.get 0
    f32.load)
  (func (export "store_load_f64") (param i64 f64) (result f64)
    local.get 0
    local.get 1
    f64.store
    local.get 0
    f64.load)
  (func (export "load_at") (param i64) (result i32)
    local.get 0
    i32.load)
  (func (export "store_offset") (param i64 i32) (result i32)
    local.get 0
    local.get 1
    i32.store offset=65532
    local.get 0
    i32.load offset=65532)
)
