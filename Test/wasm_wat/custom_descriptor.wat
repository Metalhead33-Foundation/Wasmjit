(module
  (rec
    (type $foo (descriptor $foo.desc) (struct (field i32)))
    (type $foo.desc (describes $foo) (struct (field i32)))
  )
)
