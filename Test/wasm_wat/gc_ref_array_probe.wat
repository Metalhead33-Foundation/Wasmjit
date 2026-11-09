;; Item 4, probe 19: an array whose ELEMENT is a reference
;; (docs/backends/CAPABILITY_PROBES.md section 13).
(module
  (type $refs (array (mut externref)))
  (func (export "roundtrip") (param externref) (result externref)
    local.get 0
    array.new_fixed $refs 1
    i32.const 0
    array.get $refs))
