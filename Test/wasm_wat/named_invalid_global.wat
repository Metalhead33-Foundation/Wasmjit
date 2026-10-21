;; docs/proposals/EXTENDED_NAME_SECTION.md ENS-5: a validation error that
;; names an existing global index must use its `$name`.
(module
  (global $answer i32 (i32.const 42))
  (func $writer
    (global.set $answer (i32.const 0))))
