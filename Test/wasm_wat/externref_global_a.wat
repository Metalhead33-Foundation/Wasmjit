;; docs/proposals/REFERENCE_TYPES.md RT4/RT5: A owns a mutable externref global.
;; The cell is published by `registerExports`; an importer must alias it, just
;; like the i32 shared-global pair.
(module
  (global $g (export "g") (mut externref) (ref.null extern))
  (func (export "get") (result externref) global.get $g)
  (func (export "set") (param externref) (global.set $g (local.get 0)))
  (func (export "is_null") (result i32) global.get $g ref.is_null))
