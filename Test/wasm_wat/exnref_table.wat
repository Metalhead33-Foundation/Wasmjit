;; docs/proposals/EXCEPTIONS.md EX5: an exnref is a first-class table element.
(module
  (tag $e (param i32))
  (table $t 1 exnref)
  (func (export "test") (result i32)
    (local $x exnref)
    (block $caught (result exnref)
      (try_table (catch_all_ref $caught)
        (throw $e (i32.const 42)))
      (unreachable))
    local.set $x
    (table.set $t (i32.const 0) (local.get $x))
    (ref.is_null (table.get $t (i32.const 0)))))
