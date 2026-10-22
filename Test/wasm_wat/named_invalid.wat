;; docs/proposals/EXTENDED_NAME_SECTION.md ENS-3: a named function with an
;; invalid body. wasm-tools emits the name subsection; the validator must use
;; the name in its error message.
(module
  (func $bad i32.add))
