;; Resource built-ins backed by the runtime handle table.
(component
  (type $r (resource (rep i32)))
  (canon resource.new $r (core func $new))
  (canon resource.rep $r (core func $rep))
  (canon resource.drop $r (core func $drop))
)
