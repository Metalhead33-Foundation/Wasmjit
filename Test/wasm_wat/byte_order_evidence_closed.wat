;; A private, unshared memory with no data segment: every byte in it is produced
;; at run time, which is the only case the Adaptive policy may skip conversion for.
(module
  (memory 1)
  (func (export "scratch") (result i32)
    (i32.store (i32.const 16) (i32.const 0x11223344))
    (i32.load (i32.const 16))
  )
)
