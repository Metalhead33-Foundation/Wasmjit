;; The `v128` calling convention beyond one parameter (proposals/SIMD.md S9).
;;
;; A `v128` is a 16-byte aggregate. Two of them are passed in the x86-64 ABI's
;; vector registers; the third lands on the stack, which is the case an
;; aggregate-ABI assumption would break. It is tested rather than assumed,
;; because several spec scripts pass three `v128` operands to one function.
(module
  (func (export "second") (param v128 v128 v128) (result v128)
    local.get 1)
  (func (export "third") (param v128 v128 v128) (result v128)
    local.get 2)
  (func (export "sum3") (param v128 v128 v128) (result v128)
    (i32x4.add (i32x4.add (local.get 0) (local.get 1)) (local.get 2)))
  ;; Uses the first and the *third* operand, so a broken stack slot shows up as a
  ;; wrong permutation rather than as a wrong value in the same register.
  (func (export "swizzle_third") (param v128 v128 v128) (result v128)
    (i8x16.swizzle (local.get 0) (local.get 2)))
  ;; Four: two in registers, two on the stack.
  (func (export "fourth") (param v128 v128 v128 v128) (result v128)
    local.get 3)
)
