;; A `wasip1`-shaped core module whose imports are all `wasi_snapshot_preview1`.
;; It is the fixture the hermetic stub runs: `run` writes "hi\n" through
;; `fd_write`, fills four bytes with `random_get`, reads a `clock_time_get` and
;; tries a `path_open` (which the stub must refuse); `exit` calls `proc_exit`.
;; Nothing here touches a real file, socket or clock.
(module
  (import "wasi_snapshot_preview1" "fd_write"
    (func $fd_write (param i32 i32 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "random_get"
    (func $random_get (param i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "clock_time_get"
    (func $clock_time_get (param i32 i64 i32) (result i32)))
  (import "wasi_snapshot_preview1" "path_open"
    (func $path_open
      (param i32 i32 i32 i32 i32 i64 i64 i32 i32) (result i32)))
  (import "wasi_snapshot_preview1" "proc_exit"
    (func $proc_exit (param i32)))
  (memory (export "memory") 1)
  (data (i32.const 64) "hi\n")
  (func (export "run") (result i32)
    (local $err i32)
    ;; iovec at 0: base 64, length 3
    (i32.store (i32.const 0) (i32.const 64))
    (i32.store (i32.const 4) (i32.const 3))
    (drop (call $fd_write (i32.const 1) (i32.const 0) (i32.const 1)
        (i32.const 8)))
    (drop (call $random_get (i32.const 128) (i32.const 4)))
    (drop (call $clock_time_get (i32.const 0) (i64.const 1)
        (i32.const 136)))
    (local.set $err
      (call $path_open (i32.const 3) (i32.const 0) (i32.const 64)
        (i32.const 2) (i32.const 0) (i64.const 1) (i64.const 0)
        (i32.const 0) (i32.const 140)))
    ;; (nwritten) | (path_open errno << 8) | (random_get errno << 16)
    (i32.or (i32.load (i32.const 8))
      (i32.or (i32.shl (local.get $err) (i32.const 8))
        (i32.shl (i32.load (i32.const 132)) (i32.const 16)))))
  (func (export "exit")
    (call $proc_exit (i32.const 7)))
)
