;; A reference-returning export for the generated-adapter ownership check
;; (EMBEDDING_SURFACE.md TB-11). `make` hands the caller a GC reference, which
;; owns one count; `get` reads through a borrowed reference.
(module
  (type $node (struct (field i32)))

  (func (export "make") (result (ref $node))
    (struct.new $node (i32.const 42)))

  (func (export "get") (param (ref $node)) (result i32)
    (struct.get $node 0 (local.get 0))))
