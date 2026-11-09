;; Reference ownership across the operand stack (docs/WRITING_A_BACKEND.md §6,
;; "Reference ownership: parameters, locals and the stack").
;;
;; Every instruction below moves a reference through the operand stack and
;; nothing else, so an exact reference count before and after the call pins the
;; rule "the operand stack owns one count per reference it holds". A leak shows
;; up as a count that came back one too high; a missing clone shows up as a
;; count that came back one too low, which is the dangerous direction.
;;
;; `$pair` mirrors the one in gc_refcount.wat: a scalar field then a mutable
;; reference field, so a struct can be its own child.
(module
  (type $pair (struct (field i32) (field (mut (ref null $pair)))))
  (type $refarr (array (mut (ref null $pair))))

  ;; A fresh object with a null reference field. A `struct.new*` result is
  ;; created unowned, so the caller must retain it before a safepoint.
  (func (export "make") (result (ref $pair))
    (struct.new_default $pair))

  ;; Stores `$child` into `$parent`'s reference field: `refStore`, so the field
  ;; takes its own count and the operand stack gives up the one it held.
  (func (export "set_field") (param $parent (ref $pair)) (param $child (ref $pair))
    (struct.set $pair 1 (local.get $parent) (local.get $child)))

  ;; Reads the reference out of the field and discards it. The field keeps its
  ;; own count, so the target must still be alive afterwards.
  (func (export "field_drop") (param $parent (ref $pair))
    (drop (struct.get $pair 1 (local.get $parent))))

  ;; Selects between two references and discards the result: exactly one of the
  ;; two counts goes, whichever arm the condition picks.
  (func (export "select_drop") (param $a (ref $pair)) (param $b (ref $pair))
    (param $pick i32)
    (drop (select (result (ref $pair)) (local.get $a) (local.get $b)
        (local.get $pick))))

  ;; Discards a reference parameter.
  (func (export "drop_arg") (param $a (ref $pair))
    (drop (local.get $a)))

  ;; Stores a reference into an array element, reads it back and discards it.
  ;; The element keeps its own count across both steps.
  (func (export "elem") (param $child (ref $pair))
    (local $arr (ref null $refarr))
    (local.set $arr (array.new_default $refarr (i32.const 4)))
    (array.set $refarr (local.get $arr) (i32.const 0) (local.get $child))
    (drop (array.get $refarr (local.get $arr) (i32.const 0))))

  ;; Overwrites the same local twice: the second `local.set` must release what
  ;; the first one stored, or a loop leaks one object per iteration.
  (func (export "local_overwrite") (result i32)
    (local $a (ref null $pair))
    (local.set $a (struct.new_default $pair))
    (local.set $a (struct.new_default $pair))
    (local.set $a (struct.new_default $pair))
    (i32.const 0))

  ;; The same shape in a loop, which is what a real module does: every
  ;; iteration allocates and the previous object must die.
  (func (export "local_loop") (param $n i32) (result i32)
    (local $a (ref null $pair))
    (block $done
      (loop $again
        (br_if $done (i32.eqz (local.get $n)))
        (local.set $a (struct.new_default $pair))
        (local.set $n (i32.sub (local.get $n) (i32.const 1)))
        (br $again)))
    (i32.const 0))

  ;; Returns a reference to a freshly allocated object, so the caller can pin
  ;; the count a reference result arrives with.
  (func (export "make_and_return") (result (ref $pair))
    (struct.new_default $pair))
)
