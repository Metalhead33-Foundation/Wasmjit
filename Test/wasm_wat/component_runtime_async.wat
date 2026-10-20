;; Single-agent async built-ins: stream/future creation, a blocked async read
;; completed by a synchronous write, the resulting waitable-set event, and a
;; future round trip. The core `run` returns 0 on success and a small positive
;; code identifying the first failed check.
(component
  (core module $mem (memory (export "mem") 1))
  (core instance $memory (instantiate $mem))

  (type $S (stream u8))
  (type $F (future u8))

  (canon stream.new $S (core func $snew))
  (canon stream.write $S (memory $memory "mem") (core func $swrite))
  (canon stream.read $S async (memory $memory "mem") (core func $sread))
  (canon stream.drop-readable $S (core func $sdrop))
  (canon stream.drop-writable $S (core func $sdropw))

  (canon future.new $F (core func $fnew))
  (canon future.write $F (memory $memory "mem") (core func $fwrite))
  (canon future.read $F (memory $memory "mem") (core func $fread))
  (canon future.drop-readable $F (core func $fdrop))
  (canon future.drop-writable $F (core func $fdropw))

  (canon waitable-set.new (core func $wsnew))
  (canon waitable-set.poll (memory $memory "mem") (core func $wspoll))
  (canon waitable.join (core func $wsjoin))

  (core module $m
    (import "" "mem" (memory 1))
    (import "" "snew" (func $snew (result i64)))
    (import "" "swrite" (func $swrite (param i32 i32 i32) (result i32)))
    (import "" "sread" (func $sread (param i32 i32 i32) (result i32)))
    (import "" "sdrop" (func $sdrop (param i32)))
    (import "" "sdropw" (func $sdropw (param i32)))
    (import "" "fnew" (func $fnew (result i64)))
    (import "" "fwrite" (func $fwrite (param i32 i32) (result i32)))
    (import "" "fread" (func $fread (param i32 i32) (result i32)))
    (import "" "fdrop" (func $fdrop (param i32)))
    (import "" "fdropw" (func $fdropw (param i32)))
    (import "" "wsnew" (func $wsnew (result i32)))
    (import "" "wspoll" (func $wspoll (param i32 i32) (result i32)))
    (import "" "wsjoin" (func $wsjoin (param i32 i32)))

    (func (export "run") (result i32)
      (local $packed i64) (local $r i32) (local $w i32)
      (local $ret i32) (local $ws i32) (local $ev i32)

      ;; Stream: create, join the readable end, block on an async read, then
      ;; complete it with a write and observe the waitable-set event.
      (local.set $packed (call $snew))
      (local.set $r (i32.wrap_i64 (local.get $packed)))
      (local.set $w (i32.wrap_i64 (i64.shr_u (local.get $packed) (i64.const 32))))
      (local.set $ws (call $wsnew))
      (call $wsjoin (local.get $r) (local.get $ws))

      (local.set $ret (call $sread (local.get $r) (i32.const 16) (i32.const 4)))
      (if (i32.ne (local.get $ret) (i32.const -1))
        (then (return (i32.const 1))))
      (i32.store (i32.const 0) (i32.const 0x11223344))
      (local.set $ret (call $swrite (local.get $w) (i32.const 0) (i32.const 4)))
      (if (i32.ne (local.get $ret) (i32.const 0x40))
        (then (return (i32.const 2))))

      (local.set $ev (call $wspoll (local.get $ws) (i32.const 24)))
      (if (i32.ne (local.get $ev) (i32.const 2))
        (then (return (i32.const 3))))
      (if (i32.ne (i32.load (i32.const 24)) (local.get $r))
        (then (return (i32.const 4))))
      (if (i32.ne (i32.load (i32.const 28)) (i32.const 0x40))
        (then (return (i32.const 5))))
      (if (i32.ne (i32.load (i32.const 16)) (i32.const 0x11223344))
        (then (return (i32.const 6))))
      (call $sdrop (local.get $r))
      (call $sdropw (local.get $w))

      ;; Future: write one byte, read it back, then drop both ends.
      (local.set $packed (call $fnew))
      (local.set $r (i32.wrap_i64 (local.get $packed)))
      (local.set $w (i32.wrap_i64 (i64.shr_u (local.get $packed) (i64.const 32))))
      (i32.store8 (i32.const 32) (i32.const 0x5A))
      (local.set $ret (call $fwrite (local.get $w) (i32.const 32)))
      (if (i32.ne (local.get $ret) (i32.const 0x10))
        (then (return (i32.const 7))))
      (local.set $ret (call $fread (local.get $r) (i32.const 40)))
      (if (i32.ne (local.get $ret) (i32.const 0x10))
        (then (return (i32.const 8))))
      (if (i32.ne (i32.load8_u (i32.const 40)) (i32.const 0x5A))
        (then (return (i32.const 9))))
      (call $fdrop (local.get $r))
      (call $fdropw (local.get $w))
      (i32.const 0)
    )
  )

  (core instance $ci (instantiate $m (with "" (instance
    (export "mem" (memory $memory "mem"))
    (export "snew" (func $snew))
    (export "swrite" (func $swrite))
    (export "sread" (func $sread))
    (export "sdrop" (func $sdrop))
    (export "sdropw" (func $sdropw))
    (export "fnew" (func $fnew))
    (export "fwrite" (func $fwrite))
    (export "fread" (func $fread))
    (export "fdrop" (func $fdrop))
    (export "fdropw" (func $fdropw))
    (export "wsnew" (func $wsnew))
    (export "wspoll" (func $wspoll))
    (export "wsjoin" (func $wsjoin))
  ))))

  (func (export "run") (result u32) (canon lift (core func $ci "run")))
)
