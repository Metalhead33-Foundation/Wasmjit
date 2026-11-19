;; One object, two spellings (docs/proposals/SYNTHETIC_OBJECTS.md `SO-3`).
;;
;; The module declares the `$pair` type the host interns, exports one operation
;; on `(ref $pair)` and the same two fields behind an `i64` slot, and the host's
;; handle table is what resolves the `i64` — the guest never sees a pointer and
;; never resolves anything. A handle the table has retired is the host's to
;; refuse, which is why the imports can trap.
(module
  (type $pair (struct (field i32) (field i32)))

  ;; The two directions of resolution, implemented by the test's host side.
  (import "env" "handle_get" (func $handle_get (param i64) (result i32)))
  (import "env" "handle_set" (func $handle_set (param i64) (param i32)))

  (global $saved (mut (ref null $pair)) (ref.null $pair))

  ;; ── The typed spelling: a reference the guest validates with `ref.test` ────
  (func (export "sum_pair") (param $p (ref $pair)) (result i32)
    (i32.add
      (struct.get $pair 0 (local.get $p))
      (struct.get $pair 1 (local.get $p))))

  (func (export "is_pair") (param $p anyref) (result i32)
    (ref.test (ref $pair) (local.get $p)))

  ;; ── The handle spelling: the same fields, through the host's table ────────
  ;; Field 0 is read; field 1 is written, so one pass over both surfaces proves
  ;; they reach one object rather than two that happen to agree.
  (func (export "handle_read") (param $h i64) (result i32)
    (call $handle_get (local.get $h)))

  (func (export "handle_write") (param $h i64) (param $v i32)
    (call $handle_set (local.get $h) (local.get $v)))

  ;; The guest keeps the object by reference, so releasing the host's root must
  ;; not recycle it.
  (func (export "save") (param $p (ref $pair)) (result i32)
    (global.set $saved (local.get $p))
    (i32.const 1))

  (func (export "drop_saved") (result i32)
    (global.set $saved (ref.null $pair))
    (i32.const 1)))
