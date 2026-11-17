;; The memory is exported, so another module or the host can put bytes in it or
;; read them out: the provenance is no longer the module's own business.
(module
  (memory (export "memory") 1)
  (func (export "poke") (result i32)
    (i32.store (i32.const 8) (i32.const 1))
    (i32.load (i32.const 8))
  )
)
