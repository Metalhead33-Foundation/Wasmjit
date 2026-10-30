(module
  (func (export "already_compiled") (result i32)
    i32.const 42)
  ;; The initial slice is integer-only, so a floating-point signature is
  ;; rejected while the module is compiled — after the first function has
  ;; already been emitted (docs/WRITING_A_BACKEND.md §9). Keep this unsupported:
  ;; `i32.eqz` used to serve here and is now lowered.
  (func (export "unsupported") (param f32) (result f32)
    local.get 0)
)
