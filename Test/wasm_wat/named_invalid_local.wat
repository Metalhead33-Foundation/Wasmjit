;; docs/proposals/EXTENDED_NAME_SECTION.md ENS-5: a validation error that
;; names an existing local index must use its `$name`.
(module
  (type $sig (func))
  (func $reader (local $value (ref $sig))
    (drop (local.get $value))))
