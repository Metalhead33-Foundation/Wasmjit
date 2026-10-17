;; Type imports/exports: a structural type export and an abstract resource
;; import both introduce entries into the type index space.
(component
  (type $t (record (field "x" u32)))
  (export "t" (type $t))
  (import "r" (type $r (sub resource)))
)