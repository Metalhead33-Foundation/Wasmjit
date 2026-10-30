(module
  ;; A long-running export so a second host thread can observe the MF-2 guard
  ;; while this call is inside the instance (Test/RuntimeTests.cpp).
  (func (export "spin") (param $n i32) (result i32)
    (local $i i32)
    (block $done
      (loop $l
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $l)))
    (local.get $i))

  ;; A cheap read-only export for the re-entrancy test (MF-1).
  (func (export "answer") (result i32)
    (i32.const 42)))
