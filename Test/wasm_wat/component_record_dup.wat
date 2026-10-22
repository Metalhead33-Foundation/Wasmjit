;; TM-8 first-slice fixture: the first record and the list are structurally
;; identical to types in component_types.wat, so they must share TypeIds across
;; components. The second record has the same fields in a different order and
;; must stay distinct.
(component
  (type $record (record (field "a" u32) (field "b" string)))
  (type $reordered (record (field "b" string) (field "a" u32)))
  (type $list (list u32)))
