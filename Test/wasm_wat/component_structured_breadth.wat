;; Structured component values with flat Canonical ABI signatures: record,
;; tuple, enum, flags, option, result and variant identity exports. This is the
;; runtime half of CM-27's structured breadth; the corpus execution harness
;; covers the same representation through its JSON rows when upstream provides
;; such a component.
(component
  (type $rec (record (field "a" u32)))
  (type $tup (tuple u32))
  (type $en (enum "red" "green" "blue"))
  (type $fl (flags "a" "b" "c"))
  (type $var (variant (case "none") (case "some")))
  (export $rec2 "rec" (type $rec))
  (export $tup2 "tuple" (type $tup))
  (export $en2 "enum" (type $en))
  (export $fl2 "flags" (type $fl))
  (export $var2 "variant" (type $var))

  (core module $m
    (memory (export "mem") 1)
    (func (export "one") (param i32) (result i32)
      local.get 0))

  (core instance $i (instantiate $m))
  (alias core export $i "one" (core func $one))
  (alias core export $i "mem" (core memory $mem))

  (func (export "record-id") (param "x" $rec2) (result $rec2)
    (canon lift (core func $one) (memory $mem)))
  (func (export "tuple-id") (param "x" $tup2) (result $tup2)
    (canon lift (core func $one) (memory $mem)))
  (func (export "enum-id") (param "x" $en2) (result $en2)
    (canon lift (core func $one) (memory $mem)))
  (func (export "flags-id") (param "x" $fl2) (result $fl2)
    (canon lift (core func $one) (memory $mem)))
  (func (export "variant-id") (param "x" $var2) (result $var2)
    (canon lift (core func $one) (memory $mem)))
)
