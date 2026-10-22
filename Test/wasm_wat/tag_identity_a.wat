;; Two tags with the same payload type but distinct identities. Module
;; tag_identity_b imports both (and one of them twice), so the test can assert
;; that an import aliases the exporter's tag while two declarations differ
;; (docs/proposals/EXCEPTIONS.md EX-2).
(module
  (tag (export "e") (param i32))
  (tag (export "f") (param i32))
)
