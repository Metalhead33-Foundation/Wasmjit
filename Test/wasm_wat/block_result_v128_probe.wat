;; v128 block results: fallthrough, an explicit branch to the block end, both
;; if arms, and a loop result.
(module
  (func (export "block") (result i32)
    block (result v128)
      v128.const i32x4 5 0 0 0
    end
    i32x4.extract_lane 0)

  (func (export "branch") (result i32)
    block (result v128)
      v128.const i32x4 6 0 0 0
      br 0
    end
    i32x4.extract_lane 0)

  (func (export "if") (param i32) (result i32)
    local.get 0
    if (result v128)
      v128.const i32x4 7 0 0 0
    else
      v128.const i32x4 8 0 0 0
    end
    i32x4.extract_lane 0)

  (func (export "loop") (result i32)
    loop (result v128)
      v128.const i32x4 9 0 0 0
    end
    i32x4.extract_lane 0))
