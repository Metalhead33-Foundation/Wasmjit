;; An open scheme with two abstract resource parameters. Alpha-equivalent
;; definitions must share a SchemeId independently of their nominal ids.
(component
  (type $open (component
    (import "a" (type $a (sub resource)))
    (import "b" (type $b (sub resource)))
    (export "f" (func (param "x" (own $a)) (param "y" (borrow $b))))
  ))
)
