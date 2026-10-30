;; A valid declared subtype whose field narrows `(ref null $b)` to `nullref`,
;; the concrete-subtype rule bottom-type narrowing. `wasm-tools validate
;; --features all` accepts it; the loader used to reject it (LF-2 `F3`).
(module
  (type $b (sub final (struct (field i32))))
  (type $a (sub (struct (field (ref null $b)))))
  (type $c (sub final $a (struct (field nullref))))
  (func (export "answer") (result i32) i32.const 42))
