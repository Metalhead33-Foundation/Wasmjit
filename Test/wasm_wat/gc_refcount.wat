;; A minimal GC module for the reference-counting test (docs/proposals/GC.md
;; RC-1). `$pair` is the only type, so the C++ test can allocate an instance of
;; it directly and check the header's reference count.
;;
;; `make` exercises the JIT's reference-typed store path end to end: it builds a
;; struct and stores it into its own reference-typed field, which must go
;; through the `wasm_ref_store` write barrier (retain the new value, release the
;; overwritten one) rather than a raw pointer store.
(module
  (type $pair (struct (field i32) (field (ref null any))))
  (type $arr (array (mut i32)))

  (func (export "make") (result i32)
    (local $a (ref null $pair))
    (local.set $a (struct.new_default $pair))
    (struct.set $pair 1 (local.get $a) (local.get $a))
    (i32.const 2))

  ;; Array elements are inline (D13/GC-9), so this must be exactly one heap
  ;; block, and array.len must read the inline header.
  (func (export "make_array") (param $n i32) (result i32)
    (array.len (array.new_default $arr (local.get $n)))))
