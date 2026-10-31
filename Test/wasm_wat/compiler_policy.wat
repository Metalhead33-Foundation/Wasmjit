;; The `[compilation-policy]` fixture: the `F1` shape (a self-loop reached only
;; through a conditional branch) beside a trivial export. The optimizer rejects
;; the shape at its highest level, so a `Lax` policy that asked for `Full` has a
;; failure to regress from (docs/languages/LF2_DEEP_F1_BACKEND.md §3.5,
;; docs/proposals/COMPILER_POLICY.md CP-3).
(module
  (func (export "answer") (result i32)
    i32.const 42)
  (func (export "f1") (param i32)
    local.get 0
    if
      loop
        br 0
      end
    end))
