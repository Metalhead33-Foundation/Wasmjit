;; Deliberately invalid: type $a (its own rec group) references $b, which lives
;; in a LATER group. The spec forbids forward references outside the current
;; group, and the loader must reject the module. (wasm-tools encodes this
;; without validating scope, so it is a useful negative test.)
(module
  (type $a (struct (field (ref $b))))
  (type $b (struct (field i32))))
