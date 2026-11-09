;; Item 4, probe 7: a shared memory and an atomic RMW. Accepted or refused, never
;; silent (docs/backends/CAPABILITY_PROBES.md section 1).
(module
  (memory 1 1 shared)
  (func (export "bump") (result i32)
    i32.const 0
    i32.const 1
    i32.atomic.rmw.add))
