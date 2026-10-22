;; M5 importer with a mismatching import type: (i64)->i64 vs the exporter's
;; (i32)->i32. Cross-module type matching must reject it.
(module
  (import "e" "f" (func (param i64) (result i64))))
