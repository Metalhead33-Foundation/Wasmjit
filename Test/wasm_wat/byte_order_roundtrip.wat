;; The byte-order tripwire in miniature: four byte stores read back as a word,
;; and a word store read back as a byte.
;;
;; A conforming host returns 0x44332211 and 0x44 on *any* byte order, because the
;; guest wrote the bytes little-endian by specification. A big-endian host that
;; emitted native accesses would return 0x11223344 and 0x11 instead, which is
;; exactly what docs/proposals/BIG_ENDIAN.md `BE-3` forbids.
;;
;; The last four functions are the *symmetric* half: a store and a load of the
;; same width must return the same value on every host, including a converting
;; one, which is what makes them a check on the conversion's shape rather than on
;; its presence. `halfword_signed` and `word64_signed` fail if the reversal
;; happens after the sign extension instead of before it, and `halfword_unsigned`
;; fails if the reversal widens the value.
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
  (func (export "halfword_signed") (result i32)
    (i32.store16 (i32.const 16) (i32.const 0x8001))
    (i32.load16_s (i32.const 16))
  )
  (func (export "halfword_unsigned") (result i32)
    (i32.store16 (i32.const 18) (i32.const 0x81FF))
    (i32.load16_u (i32.const 18))
  )
  (func (export "word64_signed") (result i64)
    (i64.store32 (i32.const 24) (i64.const 0x80000001))
    (i64.load32_s (i32.const 24))
  )
  (func (export "float64_bits") (result i64)
    (f64.store (i32.const 32) (f64.const 1.5))
    (i64.reinterpret_f64 (f64.load (i32.const 32)))
  )
)
