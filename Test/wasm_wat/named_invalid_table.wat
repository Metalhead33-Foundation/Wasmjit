;; docs/proposals/EXTENDED_NAME_SECTION.md ENS-5: a validation error that
;; names an existing table index must use its `$name`.
(module
  (type $sig (func))
  (table $objects 1 externref)
  (func $caller
    (call_indirect (type $sig) (i32.const 0))))
