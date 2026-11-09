;; A native import called from a frame-mode LibJIT module. The host function
;; receives (ctx, frame) and writes its i32 result after the two i32 arguments.
(module
  (import "env" "add" (func $add (param i32 i32) (result i32)))
  (func (export "run") (result i32)
    i32.const 20
    i32.const 22
    call $add))
