;; Annotation/custom-section tolerance fixture (docs/proposals/ANNOTATIONS.md ANN4).
;; A producer may synthesize a custom section from `(@custom ...)`; the engine
;; must skip it without disturbing code or data decoding.
(module
  (@custom "annotation_probe" "payload")

  (func (export "answer") (result i32)
    i32.const 42))
