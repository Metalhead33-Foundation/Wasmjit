;; Item 1: a direct self-recursive tail call. With call + return this exhausts the
;; guest stack guard; a backend that lowers it to a loop must return 0 and leave no
;; trap. Mutual/indirect chains are still call + return until the cross-function
;; trampoline lands.
(module
  (func $count (param i32) (result i32)
    local.get 0
    i32.eqz
    if (result i32)
      i32.const 0
    else
      local.get 0
      i32.const 1
      i32.sub
      return_call $count
    end)

  (func (export "run") (result i32)
    i32.const 1000000
    call $count))
