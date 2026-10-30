(module
  (type $foo (struct (field i32)))
  (func (export "f") (param (ref $foo)))
)
