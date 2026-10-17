;; Import-name kinds and the `implements` attribute.
(component
  (import "wasi:io/streams@0.2.0" (func $a))
  (import "plain" (implements "wasi:io/streams@0.2.0") (func $b))
)