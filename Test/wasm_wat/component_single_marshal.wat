;; A lowered function whose result needs guest memory must be marshalled into that
;; memory exactly once. The host-supplied `bytes` returns a `list<u8>`, which does
;; not fit the flat result limit, so the canon ABI returns it through a buffer.
;; `run` returns (length, realloc calls): the count is the point of the fixture.
(component
  (core module $host
    (memory (export "memory") 1)
  )
  (core instance $hostInstance (instantiate $host))
  (alias core export $hostInstance "memory" (core memory $memory))

  ;; The allocator the canon options name. It counts its calls, because a host
  ;; that marshals a result twice calls it twice.
  (core module $allocator
    (import "env" "memory" (memory 1))
    (global $calls (mut i32) (i32.const 0))
    (global $next (mut i32) (i32.const 1024))
    (func (export "realloc") (param i32 i32 i32 i32) (result i32)
      (global.set $calls (i32.add (global.get $calls) (i32.const 1)))
      (global.set $next (i32.add (global.get $next) (i32.const 64)))
      (global.get $next)
    )
    (func (export "calls") (result i32)
      (global.get $calls)
    )
  )
  (core instance $allocatorInstance (instantiate $allocator (with "env" (instance
    (export "memory" (memory $memory))
  ))))
  (alias core export $allocatorInstance "realloc" (core func $realloc))
  (alias core export $allocatorInstance "calls" (core func $calls))

  (import "dep" (instance $dep
    (export "bytes" (func $bytes (result (list u8))))
  ))
  (alias export $dep "bytes" (func $bytesAlias))
  (canon lower (func $bytesAlias) (memory $memory) (realloc $realloc)
    (core func $bytesLowered))

  (core module $user
    (import "env" "memory" (memory 1))
    (import "env" "bytes" (func $bytes (param i32)))
    (import "env" "calls" (func $calls (result i32)))
    ;; Returns the address of the (length, realloc calls) tuple: the lift is
    ;; indirect too, because two i32 results exceed the flat result limit.
    (func (export "run") (result i32)
      (call $bytes (i32.const 512))
      (i32.store (i32.const 600) (i32.load (i32.const 516)))
      (i32.store (i32.const 604) (call $calls))
      (i32.const 600)
    )
  )
  (core instance $userInstance (instantiate $user (with "env" (instance
    (export "memory" (memory $memory))
    (export "bytes" (func $bytesLowered))
    (export "calls" (func $calls))
  ))))
  (alias core export $userInstance "run" (core func $run))
  (func (export "run") (result (tuple u32 u32))
    (canon lift (core func $run) (memory $memory)))
)
