;; Resource, own and borrow types.
(component
  (type $r (resource (rep i32)))
  (type $own (own $r))
  (type $borrow (borrow $r))
)