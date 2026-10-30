(module
  (rec
    (type $point (descriptor $point.desc) (struct))
    (type $point.desc (describes $point) (struct)))
  (func (export "f") (result (ref $point))
    (struct.new $point)))
