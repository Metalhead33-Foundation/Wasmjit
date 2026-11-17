;; The byte-order tripwire in miniature: four byte stores read back as a word,
;; and a word store read back as a byte.
;;
;; A conforming host returns 0x44332211 and 0x44 on *any* byte order, because the
;; guest wrote the bytes little-endian by specification. A big-endian host that
;; emitted native accesses would return 0x11223344 and 0x11 instead, which is
;; exactly what docs/proposals/BIG_ENDIAN.md `BE-3` forbids.
(module
  (memory (export "memory") 1)
  (func (export "bytes_then_word") (result i32)
    (i32.store8 (i32.const 0) (i32.const 0x11))
    (i32.store8 (i32.const 1) (i32.const 0x22))
    (i32.store8 (i32.const 2) (i32.const 0x33))
    (i32.store8 (i32.const 3) (i32.const 0x44))
    (i32.load (i32.const 0))
  )
  (func (export "word_then_byte") (result i32)
    (i32.store (i32.const 8) (i32.const 0x11223344))
    (i32.load8_u (i32.const 8))
  )
)
