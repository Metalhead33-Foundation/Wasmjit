;; Exercises the component type grammar: primitives/records/variants/flags/
;; enum/tuple/list/fixed-list/option/result/map/stream/future/func types,
;; plus an import and an export.
(component
  (type $record (record (field "a" u32) (field "b" string)))
  (type $variant (variant (case "none") (case "some" u32)))
  (type $flags (flags "read" "write"))
  (type $enum (enum "a" "b"))
  (type $tuple (tuple u32 string))
  (type $list (list u32))
  (type $fixed (list u32 4))
  (type $opt (option u32))
  (type $result (result u32 (error string)))
  (type $map (map string u32))
  (type $stream (stream u32))
  (type $future (future string))
  (type $listRecord (list $record))
  (type $fn (func (param "x" u32) (result string)))
  (import "in" (func $f (type $fn)))
  (export "out" (func $f))
)