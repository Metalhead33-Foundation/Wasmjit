;; `extern.convert_any` wraps an internal reference in an **extern box**: a counted
;; GC object that starts unowned, like every fresh allocation
;; (`docs/WRITING_A_BACKEND.md` §6, "Reference ownership"). The operand stack owns
;; one count of every reference it holds, so the conversion must take that count on
;; the result and give the operand's up. A backend that replaces the operand in
;; place without doing so leaves the table pointing at a box whose count is zero:
;; reclaimed at the next safepoint, invisible in a release build whose block is not
;; reused, and silent corruption once it is.
;;
;; `churn` allocates and releases 16-byte objects -- the extern box's size class
;; (`GcHeader::kExternBoxBytes`) -- so a box that was reclaimed by mistake is handed
;; out again and its type id overwritten by an empty struct's. That makes the defect
;; visible in every configuration, not only under the Debug build's poison-on-free.
(module
  (type $cell (struct (field i32)))
  ;; Empty: exactly kExternBoxBytes (8-byte type header + one pointer), so churn
  ;; reuses a freed box's block rather than a neighbouring size class.
  (type $empty (struct))

  (table $te 4 externref)

  ;; Two boxes with differently-owned inners: an uncounted i31 immediate, and a
  ;; struct whose count the box holds; plus a null, which converts to a null extern.
  (func (export "fill")
    (table.set $te (i32.const 0) (extern.convert_any (ref.i31 (i32.const 8))))
    (table.set $te (i32.const 1) (extern.convert_any (struct.new $cell (i32.const 5))))
    (table.set $te (i32.const 2) (extern.convert_any (ref.null any)))
  )

  ;; Allocate-and-release of the box's size class, `$rounds` times.
  (func (export "churn") (param $rounds i32) (result i32)
    (local $i i32)
    (local $sink i32)
    (block $done
      (loop $again
        (br_if $done (i32.ge_u (local.get $i) (local.get $rounds)))
        (local.set $sink
          (i32.add (local.get $sink)
            (ref.test (ref $empty) (struct.new_default $empty))))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $again)
      )
    )
    (local.get $sink)
  )

  ;; The box must still be an extern after the churn.
  (func (export "test_boxed") (param $i i32) (result i32)
    (ref.test externref (table.get $te (local.get $i)))
  )
  (func (export "test_nonnull") (param $i i32) (result i32)
    (ref.test (ref extern) (table.get $te (local.get $i)))
  )

  ;; And it must still own its inner reference.
  (func (export "unbox_i31") (result i32)
    (i31.get_u (ref.cast (ref i31)
      (any.convert_extern (table.get $te (i32.const 0)))))
  )
  (func (export "unbox_field") (result i32)
    (struct.get $cell 0 (ref.cast (ref $cell)
      (any.convert_extern (table.get $te (i32.const 1)))))
  )
  (func (export "unbox_null_is_null") (result i32)
    (ref.is_null (any.convert_extern (table.get $te (i32.const 2))))
  )
)
