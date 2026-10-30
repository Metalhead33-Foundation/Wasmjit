(module
  (rec
    (type $counter (descriptor $counter.vtable) (struct (field i32)))
    (type $counter.vtable (describes $counter) (struct (field externref))))
  (import "env" "proto" (global $proto externref))
  (global $vtable (ref (exact $counter.vtable))
    (struct.new $counter.vtable (global.get $proto)))
  (global $counter (ref (exact $counter))
    (struct.new_default_desc $counter (global.get $vtable)))
  (func (export "counter") (result (ref $counter))
    (global.get $counter)))
