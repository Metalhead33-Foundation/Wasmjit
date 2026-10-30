;; Host-facing continuation API: export a fresh continuation as a `(ref cont)`
;; value; the host receives its lowered `void*` and wraps it.
(module
  (type $ft (func (result i32)))
  (type $ct (cont $ft))

  (func $gen (type $ft)
    i32.const 99)

  (elem declare func $gen)

  (func (export "make") (result (ref $ct))
    ref.func $gen
    cont.new $ct))
