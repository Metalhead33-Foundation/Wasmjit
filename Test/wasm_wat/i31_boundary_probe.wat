;; `GAP-7`: what an `i31ref` looks like at the C boundary. An `i31` is an
;; *unboxed immediate* — `(value << 1) | 1` — so a host receives a word whose low
;; bit is set. It is a legal opaque token (it round-trips unchanged) and never a
;; pointer: a host must not dereference it or hand it to foreign code that would.
;; `refClone`/`refDrop` treat it as uncounted, because it has no header.
(module
  (func (export "make") (param i32) (result (ref i31))
    (ref.i31 (local.get 0)))

  (func (export "unwrap") (param (ref i31)) (result i32)
    (i31.get_s (local.get 0))))
