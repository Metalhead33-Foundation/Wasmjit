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
  (export "memory64-f" (func $lifted))

  ;; Memory64 pointer width reaches the flattening of pointer-shaped values:
  ;; a string parameter is (i64 pointer, i32 length), not (i32, i32).
  (core module $G
    (func (export "g") (param i64 i32) (result i32) (i32.const 9)))
  (core instance $gi (instantiate $G))
  (alias core export $gi "g" (core func $g))
  (type $gft (func (param "s" string) (result u32)))
  (canon lift (core func $g) (memory $mem) (func $lifted_string (type $gft)))
)
