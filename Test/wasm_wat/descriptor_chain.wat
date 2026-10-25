(module
  (rec
    (type $foo (descriptor $foo.desc) (struct))
    (type $foo.desc (describes $foo) (descriptor $meta) (struct))
    (type $meta (describes $foo.desc) (struct))
  )
)
