;; docs/DIAGNOSTICS.md: a module that logs through the host diagnostics
;; imports. The strings live in linear memory; the imports copy them into a
;; structured record. `env.panic` also traps, so a unit test can fail loudly
;; from inside the guest.
(module
  (import "env" "info"  (func $info  (param i32 i32)))
  (import "env" "warn"  (func $warn  (param i32 i32)))
  (import "env" "error" (func $error (param i32 i32)))
  (import "env" "panic" (func $panic (param i32 i32)))

  (memory (export "memory") 1)

  (data (i32.const 0)  "info from guest")
  (data (i32.const 32) "warn from guest")
  (data (i32.const 64) "error from guest")
  (data (i32.const 96) "panic from guest")
  ;; A lone 0xFF is not valid UTF-8: the JSON form must replace it, not throw.
  (data (i32.const 160) "\ff")

  (func (export "log_all")
    (call $info  (i32.const 0)  (i32.const 15))
    (call $warn  (i32.const 32) (i32.const 15))
    (call $error (i32.const 64) (i32.const 16)))

  (func (export "do_panic")
    (call $panic (i32.const 96) (i32.const 16)))

  ;; The range is deliberately past the end of the single page: the import must
  ;; report a placeholder, not read (docs/SAFETY.md §2.2).
  (func (export "bad_range")
    (call $info (i32.const 65000) (i32.const 1000)))

  ;; 5000 bytes are readable but exceed kMaxDiagnosticMessageBytes (4096), so
  ;; the record must be truncated instead of trusting the guest's length.
  (func (export "truncated")
    (call $info (i32.const 0) (i32.const 5000)))

  ;; One byte that is not valid UTF-8; the JSON form replaces it with U+FFFD.
  (func (export "invalid_utf8")
    (call $info (i32.const 160) (i32.const 1))))
