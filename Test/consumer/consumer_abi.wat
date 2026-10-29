;; The embedder-facing signatures the installed headers must support
;; (docs/EMBEDDING_SURFACE.md ES-7, closing AAS1's consumer-coverage row):
;; f64 by value, a multi-result tuple, a single v128 result and a mixed
;; (i32, v128) tuple. Test/install-check.sh assembles it with wat2wasm and the
;; out-of-tree consumer calls it through <wasmjit/WasmJit.hpp> only.
(module
  (func (export "half") (param f64) (result f64)
    local.get 0
    f64.const 2
    f64.div)

  (func (export "divmod") (param i32 i32) (result i32 i32)
    local.get 0
    local.get 1
    i32.div_s
    local.get 0
    local.get 1
    i32.rem_s)

  (func (export "splat") (param i32) (result v128)
    local.get 0
    i32x4.splat)

  (func (export "mix") (param i32 v128) (result i32 v128)
    local.get 0
    local.get 1)
)
