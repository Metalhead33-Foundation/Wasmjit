;; Item 4, probe 12: two memories, indexed access
;; (docs/backends/CAPABILITY_PROBES.md section 7).
(module
  (memory 1)
  (memory 1)
  (func (export "second_size") (result i32)
    memory.size 1))
