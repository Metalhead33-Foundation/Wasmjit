(module
  (func (export "already_compiled") (result i32)
    i32.const 42)
  ;; The scalar slice excludes v128, so a vector signature is rejected while the
  ;; module is compiled — after the first function has already been emitted
  ;; (docs/WRITING_A_BACKEND.md §9). Floats used to serve here and are now
  ;; lowered, so the probe must stay one proposal ahead of the slice.
  (func (export "unsupported") (param v128) (result v128)
    local.get 0)
)
