;; Reference chunks 1-3: ref.null, ref.is_null, ref.eq, ref.func and
;; ref.as_non_null. A reference is an opaque word: SLJIT resolves the Callable
;; at run time, AsmJit uses the function's own code address, so only nullness
;; and equality are observed here. `ref.eq` is lowered but unfixtured: it needs
;; a `(ref null eq)` operand, and constructing one requires GC types.
(module
  (func $f0 (export "f0") (result i32) i32.const 0)
  (func $f1 (export "f1") (result i32) i32.const 1)
  (func (export "null_is_null") (result i32)
    ref.null func
    ref.is_null)
  (func (export "null_extern_is_null") (result i32)
    ref.null extern
    ref.is_null)
  (func (export "func_not_null") (result i32)
    ref.func $f0
    ref.is_null)
  (func (export "func_as_non_null") (result i32)
    ref.func $f0
    ref.as_non_null
    ref.is_null)
  (func (export "as_non_null_traps") (result i32)
    ref.null func
    ref.as_non_null
    drop
    i32.const 1)
  (func (export "br_on_null_null") (result i32) (local i32)
    block
      ref.null func
      br_on_null 0
      drop
      i32.const 7
      local.set 0
    end
    local.get 0)
  (func (export "br_on_null_func") (result i32) (local i32)
    block
      ref.func $f0
      br_on_null 0
      drop
      i32.const 7
      local.set 0
    end
    local.get 0)
)
