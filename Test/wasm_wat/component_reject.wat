;; A minimal Component Model component: preamble only, no sections.
;;
;; This is the `(component)` unit, which encodes as
;;   00 61 73 6D   magic
;;   0D 00         version (pre-standard)
;;   01 00         layer = 1  (0 = core module)
;;
;; `Test/main.cpp` loads it and asserts that the loader rejects it with the
;; component-specific message instead of misparsing the (nonexistent)
;; sections as core ones. See docs/proposals/COMPONENT_MODEL.md §2.2 / CM-0.
(component)
