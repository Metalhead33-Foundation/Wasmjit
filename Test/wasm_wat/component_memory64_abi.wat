;; CM-20: a 64-bit core memory used by a canonopt is gated by Memory64.
(component
  (core module $Mem (memory (export "mem") i64 1))
  (core instance $memInst (instantiate $Mem))
  (alias core export $memInst "mem" (core memory $mem))
  (core module $F (func (export "f") (result i32) (i32.const 0)))
  (core instance $fi (instantiate $F))
  (alias core export $fi "f" (core func $f))
  (type $ft (func (result u32)))
  (canon lift (core func $f) (memory $mem) (func $lifted (type $ft)))
)
