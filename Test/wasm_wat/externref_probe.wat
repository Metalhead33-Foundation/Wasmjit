;; Item 4's second probe: an externref parameter and result. Under the
;; no-partial-modules ruling the only acceptable answers are "round-trips" or
;; "refused with a reason" (docs/API_ABI_STABILITY.md).
(module
  (func (export "roundtrip") (param externref) (result externref)
    local.get 0))
