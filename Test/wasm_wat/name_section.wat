;; Extended name section fixture (docs/proposals/EXTENDED_NAME_SECTION.md).
;; wasm-tools emits the standard and extended subsections from these bindings:
;;   0 module, 1 function, 2 local, 3 label, 4 type, 5 table, 6 memory,
;;   7 global, 8 element, 9 data, 10 field, 11 tag.
(module $mod
  (type $ft (func (param $p i32) (result i32)))
  (type $st (struct (field $a i32) (field $b i64)))
  (type $tagt (func (param i32)))

  (memory $mem 1)
  (table $tab 1 funcref)
  (global $g i32 (i32.const 7))

  (func $f (type $ft) (param $p i32) (result i32)
    (block $lbl (result i32)
      local.get $p))

  (elem $e func $f)
  (data $d "hi")
  (tag $tag (type $tagt)))
