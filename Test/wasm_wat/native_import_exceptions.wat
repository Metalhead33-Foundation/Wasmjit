;; docs/ABI.md "Writing an import" and docs/proposals/EXCEPTIONS.md EXD2:
;; a native import is called from inside generated code, so the two failure
;; channels must stay disjoint. A guest exception that is *caught* is a value
;; (and its pending slot is already clear when the import runs); a trap or a
;; C++ exception raised by an import is never caught by `try_table`; and an
;; uncaught guest exception re-propagates at the call site before the next
;; import can run.
(module
  (import "env" "observe" (func $observe (param i32)))
  (import "env" "observe_ref" (func $observe_ref (param i32 exnref)))
  (import "env" "fail_trap" (func $fail_trap))
  (import "env" "fail_throw" (func $fail_throw))

  (tag $e (param i32))

  ;; The catch clears the pending slot, then the payload is handed to C++.
  (func (export "catch-and-observe") (param i32) (result i32)
    (local $payload i32)
    (block $h (result i32)
      (try_table (catch $e $h)
        (throw $e (local.get 0)))
    )
    (local.set $payload)
    (call $observe (local.get $payload))
    (i32.const 7))

  ;; The exnref itself crosses the boundary; the host can read its tag and
  ;; payload with `wasm_exn_tag_id` and the payload layout.
  (func (export "catch-ref-and-observe") (param i32) (result i32)
    (block $h (result i32 exnref)
      (try_table (catch_ref $e $h)
        (throw $e (local.get 0)))
    )
    (call $observe_ref)
    (i32.const 8))

  ;; A native trap is not a Wasm exception: `catch_all` must not see it.
  (func (export "catch-native-trap") (result i32)
    (block $handler
      (try_table (catch_all $handler)
        (call $fail_trap)
        (return (i32.const 99))))
    (i32.const 98))

  ;; A C++ exception is converted to a trap by `nativeImport`, so it is not a
  ;; Wasm exception either.
  (func (export "catch-native-exception") (result i32)
    (block $handler
      (try_table (catch_all $handler)
        (call $fail_throw)
        (return (i32.const 99))))
    (i32.const 98))

  ;; An uncaught exception leaves the call site's pending check set, so the
  ;; following import must never run.
  (func $thrower (param i32)
    (throw $e (local.get 0)))
  (func (export "propagate-past-import") (param i32) (result i32)
    (call $thrower (local.get 0))
    (call $observe (i32.const 1234))
    (i32.const 1))
)
