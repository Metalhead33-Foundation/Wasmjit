;; docs/proposals/EXTENDED_NAME_SECTION.md ENS-5: a trapping function whose
;; name the engine must include in the runtime trap message. The unnamed
;; function proves the numeric index is still reported when no name exists.
(module
  (func $boom (export "boom")
    unreachable)
  (func (export "plain")
    unreachable))
