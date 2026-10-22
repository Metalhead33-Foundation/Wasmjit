;; A local record must be exported before a function export may expose it in a
;; function signature; the export introduces the externally-visible name.
(component
  (type $r (record (field "x" u32)))
  (export $r2 "r" (type $r))
  (core module $m
    (func (export "f") (param i32) (result i32)
      (local.get 0)
    )
  )
  (core instance $i (instantiate $m))
  (alias core export $i "f" (core func $f))
  (type $ft (func (param "x" $r2) (result $r2)))
  (canon lift (core func $f) (func $lifted (type $ft)))
  (export "f" (func $lifted))
)
