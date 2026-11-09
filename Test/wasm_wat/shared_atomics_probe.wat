;; Item 3: shared-everything atomics over numeric slots (SET-3). The linear-memory
;; family is covered by atomics_ops_probe; this fixture checks the address-forming
;; forms: a shared global, a struct field and an array element. Reference slots and
;; tables are refused by the backends that cannot honour the GC write barrier.
;; (wasm-tools validate needs --features shared-everything-threads for this file.)
(module
  (global $g (shared mut i32) (i32.const 0))
  (type $s (struct (field (mut i32)) (field (mut i64))))
  (type $a (array (mut i32)))

  (func (export "global_probe") (result i32)
    i32.const 5
    global.atomic.rmw.add acq_rel $g
    drop
    global.atomic.get seq_cst $g)

  (func (export "struct_probe") (result i32) (local $x (ref null $s))
    struct.new_default $s
    local.set $x
    local.get $x
    i32.const 7
    struct.atomic.set seq_cst $s 0
    local.get $x
    struct.atomic.get seq_cst $s 0)

  (func (export "struct64_probe") (result i64) (local $x (ref null $s))
    struct.new_default $s
    local.set $x
    local.get $x
    i64.const 9
    struct.atomic.set seq_cst $s 1
    local.get $x
    struct.atomic.get seq_cst $s 1)

  (func (export "array_probe") (result i32) (local $x (ref null $a))
    i32.const 1
    array.new_default $a
    local.set $x
    local.get $x
    i32.const 0
    i32.const 11
    array.atomic.set seq_cst $a
    local.get $x
    i32.const 0
    array.atomic.get seq_cst $a))
