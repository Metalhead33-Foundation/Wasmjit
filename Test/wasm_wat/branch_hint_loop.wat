;; Branch hinting (proposal branch-hinting, Phase 5 / Wasm 3.0). The
;; `@metadata.code.branch_hint` annotation becomes a `metadata.code.branch_hint`
;; custom section in the binary. It is a *pure optimisation hint*: an engine may
;; act on it or ignore it, with no effect on results, traps or validation.
;;
;; The annotation is attached to the loop-exit `br_if`, whose target is a
;; forward block. "\01" means the condition is likely true. Test/main.cpp uses
;; this module to check that (a) the hint section is parsed and rebased onto the
;; function body, and (b) acting on it leaves the computed result unchanged.
(module
  (func (export "sum_upto") (param $n i32) (result i32)
    (local $i i32) (local $total i32)

    block $exit
      loop $L
        local.get $i
        local.get $n
        i32.ge_s
        (@metadata.code.branch_hint "\01")
        br_if $exit

        local.get $i
        i32.const 1
        i32.add
        local.set $i

        local.get $total
        local.get $i
        i32.add
        local.set $total

        br $L
      end
    end

    local.get $total))
