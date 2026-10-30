;; docs/proposals/PROFILES.md PR-2: the `T` marker excludes atomic instructions.
(module
  (memory 1 1)
  (func (export "load") (result i32)
    (i32.atomic.load (i32.const 0))))
