;; Global reference atomics: an unshared mutable eqref global. Set/xchg/cmpxchg
;; go through the reference barrier; the final get observes a null eqref.
(module
  (global $g (mut eqref) (ref.null eq))

  (func (export "run") (param eqref) (result i32)
    local.get 0
    global.atomic.set seq_cst $g
    local.get 0
    global.atomic.rmw.xchg seq_cst $g
    drop
    local.get 0
    local.get 0
    global.atomic.rmw.cmpxchg seq_cst $g
    drop
    global.atomic.get seq_cst $g
    ref.is_null))
