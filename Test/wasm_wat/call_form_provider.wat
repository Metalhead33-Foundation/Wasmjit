;; A provider module that is forced into the staging-frame call form by a *wide*
;; function, and that also exports a narrow one.
;;
;; This is the shape that exposes the module-wide call-form rule: `wide` alone
;; decides the form of `narrow` as well, so a caller compiled in the register
;; form reaches `narrow` through a `void fn(VMContext*, void* frame)` entry.
;; Six parameters exceed every register-form argument ceiling in this tree.
(module
  (func (export "wide") (param i32 i32 i32 i32 i32 i32) (result i32)
    (i32.add (local.get 0) (local.get 1)))

  ;; Narrow, and therefore expressible in the register form -- but compiled in
  ;; the module's form, whatever that is.
  (func (export "narrow") (param i32) (result i32)
    (i32.mul (local.get 0) (i32.const 3)))
)
