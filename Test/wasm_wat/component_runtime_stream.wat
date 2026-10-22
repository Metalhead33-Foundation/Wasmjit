;; Stream/future handles are opaque runtime table entries. This slice exercises
;; allocation and drop; read/write/copy semantics are not implemented yet.
(component
  (type $s (stream u32))
  (canon stream.new $s (core func $new))
  (canon stream.drop-readable $s (core func $dr))
  (canon stream.drop-writable $s (core func $dw))
)
