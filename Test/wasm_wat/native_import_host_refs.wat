;; docs/ABI.md "Native (host) function imports" and `HostBridge`: a host-owned
;; pointer returned by an import is an ordinary `externref`. It survives a
;; global, comes back to another import unchanged, and the bridge's
;; retain/release callbacks participate in the global's write barrier.
(module
  (import "env" "get_handle" (func $get_handle (result externref)))
  (import "env" "read_handle" (func $read_handle (param externref) (result i32)))
  (import "env" "same_handle" (func $same_handle (param externref externref) (result i32)))

  (global $slot (mut externref) (ref.null extern))

  ;; Import -> global -> import: the pointer must be the same object.
  (func (export "round-trip") (result i32)
    (global.set $slot (call $get_handle))
    (call $read_handle (global.get $slot)))

  ;; Two import results compare equal without any engine-side conversion.
  (func (export "identity") (result i32)
    (call $same_handle (call $get_handle) (call $get_handle)))

  (func (export "is-null") (result i32)
    (global.get $slot)
    (ref.is_null))

  (func (export "clear")
    (global.set $slot (ref.null extern)))
)
