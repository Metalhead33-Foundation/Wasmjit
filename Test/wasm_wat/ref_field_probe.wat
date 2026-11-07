;; Item 4, probe 16: a reference stored in a struct field
;; (docs/backends/CAPABILITY_PROBES.md section 9).
(module
  (type $box (struct (field externref)))
  (func (export "roundtrip") (param externref) (result externref)
    local.get 0
    struct.new $box
    struct.get $box 0))
