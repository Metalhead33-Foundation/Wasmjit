;; Canonical ABI flat-shape fixture: record/tuple/fixed-list/list parameters.
(component
  (type $record (record (field "a" u32) (field "b" string)))
  (type $tuple (tuple u32 string))
  (type $fixed (list u32 4))
  (type $list (list u32))
  (type $fn (func (param "r" $record) (param "f" $fixed) (result $tuple)))
  (type $fnList (func (param "l" $list)))
)
