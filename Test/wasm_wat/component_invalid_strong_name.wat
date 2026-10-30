;; Strong uniqueness: `foo` and `FOO` canonicalize to the same name.
(component
  (import "foo" (func))
  (import "FOO" (func))
)
