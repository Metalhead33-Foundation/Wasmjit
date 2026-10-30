;; Imports tag_identity_a's i32-payload tag with an i64 payload: the import must
;; be rejected at link time (docs/proposals/EXCEPTIONS.md EX-2).
(module
  (tag (import "a" "e") (param i64))
)
