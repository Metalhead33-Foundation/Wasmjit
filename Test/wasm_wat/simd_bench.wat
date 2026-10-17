;; The SIMD-L8 benchmark workload (docs/SIMD_LIBJIT.md §8.2). One fixed loop,
;; six `v128` ops per iteration, mixing memory and arithmetic so that both the
;; helper calls and the producer/consumer slot traffic appear:
;;
;;   v128.load 0 ─┐
;;   v128.load 16 ┴ i32x4.add      (2 loads + 1 op)
;;   v128.load 32 ─┐
;;                ┴ i32x4.mul      (1 load + 1 op)
;;   acc ──────────┴ i32x4.add      (1 op)
;;
;; The arithmetic folds to  acc += (a + b) * c  per iteration, so the expected
;; result is exact and every benchmark variant can be cross-checked against it:
;; with a = [1,2,3,4], b = [5,6,7,8], c = [9,10,11,12] and acc0 = [1,2,3,4],
;; lane l advances by ((a+b)*c)[l] = [54, 80, 110, 144] and lane 0 ends at
;; 1 + n*54.
;;
;; Kept deliberately small in ops-per-byte: the point is to measure the cost of
;; *crossing the helper boundary*, which dominates only when the loop body is
;; short. A long body would hide it (docs/SIMD_LIBJIT.md §8.1).
(module
  (memory (export "memory") 1)
  (data (i32.const 0)
    "\01\00\00\00\02\00\00\00\03\00\00\00\04\00\00\00"
    "\05\00\00\00\06\00\00\00\07\00\00\00\08\00\00\00"
    "\09\00\00\00\0a\00\00\00\0b\00\00\00\0c\00\00\00")

  (func (export "bench") (param $n i32) (result i32)
    (local $i i32)
    (local $acc v128)
    (local.set $acc (v128.const i32x4 1 2 3 4))
    (block $done
      (loop $again
        (local.set $acc
          (i32x4.add
            (i32x4.mul
              (i32x4.add (v128.load (i32.const 0)) (v128.load (i32.const 16)))
              (v128.load (i32.const 32)))
            (local.get $acc)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br_if $again (i32.lt_u (local.get $i) (local.get $n)))))
    (i32x4.extract_lane 0 (local.get $acc))))
