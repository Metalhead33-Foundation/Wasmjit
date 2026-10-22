;; docs/proposals/JS_INTEGRATION.md JS-1: the host bridge must be visible to
;; imports called from a module's start function. `record_start` observes the
;; per-instance host state published by HostBridge::instanceCreated.
(module
  (import "env" "record_start" (func $record_start (param i32)))
  (func $start
    i32.const 1
    call $record_start)
  (start $start)
  (func (export "answer") (result i32)
    i32.const 42))
