;; docs/DIAGNOSTICS.md: a module that logs through the host diagnostics
;; imports. The strings live in linear memory; the imports copy them into a
;; structured record. `env.panic` also traps, so a unit test can fail loudly
;; from inside the guest.
(module
  (import "env" "info"   (func $info   (param i32 i32)))
  (import "env" "warn"   (func $warn   (param i32 i32)))
  (import "env" "error"  (func $error  (param i32 i32)))
  (import "env" "panic"  (func $panic  (param i32 i32)))
  ;; The one-function enum form and the AssemblyScript String shape.
  (import "env" "log"    (func $log    (param i32 i32 i32)))
  (import "env" "log_as" (func $log_as (param i32 i32)))
  ;; The stringref entry, misdeclared as two integers: a forged handle must be
  ;; refused by the host, never dereferenced (docs/SAFETY.md).
  (import "env" "log_str" (func $log_str_forged (param i32 i32)))

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
    (call $info (i32.const 160) (i32.const 1)))

  ;; A fake AssemblyScript `String`: the managed-object header's `rtSize` (the
  ;; UTF-16LE payload byte length) at 220, the 13 code units at 224. This is the
  ;; documented AS 0.28 rt/common.ts layout the host reads.
  (data (i32.const 220)
    "\1a\00\00\00"
    "\61\00\73\00\20\00\66\00\72\00\6f\00\6d\00\20\00\67\00\75\00\65\00\73\00\74\00")


  ;; The enum form with a zero length: each pointer is a NUL-terminated C
  ;; string, so the host scans to the NUL instead of trusting a length.
  (func (export "log_enum_all")
    (call $log (i32.const 0) (i32.const 0)  (i32.const 0))
    (call $log (i32.const 1) (i32.const 32) (i32.const 0))
    (call $log (i32.const 2) (i32.const 64) (i32.const 0))
    ;; An unknown selector is an invalid-kind record, not a crash.
    (call $log (i32.const 9) (i32.const 0)  (i32.const 0)))

  ;; `kind = 3` is the panic: a Fatal record, then a trap.
  (func (export "log_enum_panic")
    (call $log (i32.const 3) (i32.const 96) (i32.const 0)))

  ;; The AssemblyScript shape: a valid object, then a pointer below the header.
  (func (export "log_as_good")
    (call $log_as (i32.const 0) (i32.const 224)))

  (func (export "log_as_bad")
    (call $log_as (i32.const 0) (i32.const 3)))

  (func (export "log_str_forged")
    (call $log_str_forged (i32.const 0) (i32.const 8))))
