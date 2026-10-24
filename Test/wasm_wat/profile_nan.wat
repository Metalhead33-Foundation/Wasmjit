;; docs/proposals/PROFILES.md PR-3: every place a scalar float becomes observable
;; to the host (reinterpret, store, global.set, return, multi-value return).
;; A NaN payload written with `reinterpret` must be canonical in deterministic mode.
(module
  (memory (export "mem") 1)
  (global $g (export "g") (mut f32) (f32.const 0))

  (func (export "reinterpret_roundtrip") (param i32) (result i32)
    (i32.reinterpret_f32 (f32.reinterpret_i32 (local.get 0))))

  (func (export "store_nan") (param i32)
    (f32.store (i32.const 0) (f32.reinterpret_i32 (local.get 0))))

  (func (export "set_global_nan") (param i32)
    (global.set $g (f32.reinterpret_i32 (local.get 0))))

  (func (export "nan_result") (param i32) (result f32)
    (f32.reinterpret_i32 (local.get 0)))

  (func (export "nan_pair") (param i32) (result f32 f32)
    (f32.reinterpret_i32 (local.get 0))
    (f32.reinterpret_i32 (local.get 0)))

  (func (export "reinterpret_roundtrip64") (param i64) (result i64)
    (i64.reinterpret_f64 (f64.reinterpret_i64 (local.get 0))))

  (func (export "store_nan64") (param i64)
    (f64.store (i32.const 8) (f64.reinterpret_i64 (local.get 0)))))
