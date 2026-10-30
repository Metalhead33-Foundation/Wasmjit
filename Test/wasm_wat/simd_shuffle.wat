;; SIMD-L2: `i8x16.shuffle` and `i8x16.swizzle` through the JIT
;; (docs/SIMD_LIBJIT.md §10). These two carry the hazards the note calls out:
;;   Y4 — shuffle indices 0-15 read the *first* operand, 16-31 the *second*;
;;   Y3 — swizzle zeroes every index >= 16, where pshufb would alias it to
;;        `index & 15` (index 127 in particular is the silent-failure case).
;;
;; `a` is the 16 bytes 1..16 and `b` is 129..144, so a lane value identifies
;; which operand and which lane it came from.
(module
  (func $a (result v128) (v128.const i8x16 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16))
  (func $b (result v128) (v128.const i8x16 129 130 131 132 133 134 135 136
                                                137 138 139 140 141 142 143 144))
  (func $interleave (result v128)
    (i8x16.shuffle 0 16 1 17 2 18 3 19 4 20 5 21 6 22 7 23 (call $a) (call $b)))
  (func $reversed (result v128)
    (i8x16.shuffle 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 (call $a) (call $b)))
  (func $hazardIdx (result v128)
    (v128.const i8x16 0 1 16 127 128 255 2 3 4 5 6 7 8 9 10 11))

  ;; Interleaved: even lanes from `a`, odd lanes from `b`.
  (func (export "shuffle_u8_0") (result i32)
    (i8x16.extract_lane_u 0 (call $interleave)))
  (func (export "shuffle_u8_1") (result i32)
    (i8x16.extract_lane_u 1 (call $interleave)))
  (func (export "shuffle_u8_15") (result i32)
    (i8x16.extract_lane_u 15 (call $interleave)))

  ;; Fully reversed: lane 0 comes from the *second* operand's last lane.
  (func (export "shuffle_rev_u8_0") (result i32)
    (i8x16.extract_lane_u 0 (call $reversed)))
  (func (export "shuffle_rev_u8_15") (result i32)
    (i8x16.extract_lane_u 15 (call $reversed)))

  ;; Swizzle: indices 0 and 1 are in range, 16/127/128/255 are not.
  (func (export "swizzle_u8_0") (result i32)
    (i8x16.extract_lane_u 0 (i8x16.swizzle (call $a) (call $hazardIdx))))
  (func (export "swizzle_u8_1") (result i32)
    (i8x16.extract_lane_u 1 (i8x16.swizzle (call $a) (call $hazardIdx))))
  (func (export "swizzle_u8_2") (result i32)
    (i8x16.extract_lane_u 2 (i8x16.swizzle (call $a) (call $hazardIdx))))
  (func (export "swizzle_u8_3") (result i32)
    (i8x16.extract_lane_u 3 (i8x16.swizzle (call $a) (call $hazardIdx))))
  (func (export "swizzle_u8_4") (result i32)
    (i8x16.extract_lane_u 4 (i8x16.swizzle (call $a) (call $hazardIdx))))
  (func (export "swizzle_u8_5") (result i32)
    (i8x16.extract_lane_u 5 (i8x16.swizzle (call $a) (call $hazardIdx))))
  ;; ... and lane 6 is in range again (index 2), so the zeroing is per lane.
  (func (export "swizzle_u8_6") (result i32)
    (i8x16.extract_lane_u 6 (i8x16.swizzle (call $a) (call $hazardIdx)))))
