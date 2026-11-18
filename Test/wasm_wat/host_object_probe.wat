;; A host-created object (docs/proposals/SYNTHETIC_OBJECTS.md `SO-1`): the module
;; declares the same leaf struct the host interns, so the Store gives both one
;; `TypeId` and the guest can inspect the object the host allocated.
;;
;; The fields are scalars on purpose: a host type with a reference field cannot
;; be interned yet (`SO17`), and this fixture is the evidence for the seam, not
;; for the graph walk.
(module
  (type $pair (struct (field i32) (field i32)))
  (global $saved (mut (ref null $pair)) (ref.null $pair))

  ;; Reads both fields: a layout or type mismatch shows up as a wrong sum.
  (func (export "sum_pair") (param $p (ref $pair)) (result i32)
    (i32.add
      (struct.get $pair 0 (local.get $p))
      (struct.get $pair 1 (local.get $p))))

  ;; The header's `TypeId` is what `ref.test` reads, so this is the identity
  ;; assertion made by the guest rather than by the host.
  (func (export "is_pair") (param $p anyref) (result i32)
    (ref.test (ref $pair) (local.get $p)))

  ;; Stores the reference in a global: the write barrier takes a count, which
  ;; must keep the object alive after the host releases its root.
  (func (export "save") (param $p (ref $pair)) (result i32)
    (global.set $saved (local.get $p))
    (i32.const 1))

  ;; Releases the global's count; the object dies at the next safepoint.
  (func (export "drop_saved") (result i32)
    (global.set $saved (ref.null $pair))
    (i32.const 1)))
