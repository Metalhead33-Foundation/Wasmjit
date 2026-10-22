;; Imports tag_identity_a's two tags, with an alias of the first. The tag index
;; space is therefore [e, e-alias, f]: the first two must resolve to the same
;; process-wide TagId, the third to a different one.
(module
  (tag $e       (import "a" "e") (param i32))
  (tag $e_alias (import "a" "e") (param i32))
  (tag $f       (import "a" "f") (param i32))
)
