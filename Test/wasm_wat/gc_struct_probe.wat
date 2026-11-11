;; Item 4, probe 6: a GC struct type crosses the boundary. Accepted or refused, never
;; silent (docs/TESTING.md section 1).
(module
  (type $pair (struct (field i32) (field i32)))
  (func (export "make") (result (ref $pair))
    i32.const 1
    i32.const 2
    struct.new $pair))
