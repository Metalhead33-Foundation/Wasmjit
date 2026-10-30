;; Canonical lower with a string-encoding option.
(component
  (type $ft (func (param "x" u32) (result u32)))
  (import "f" (func $f (type $ft)))
  (canon lower (func $f) string-encoding=utf8 (core func $cf))
)