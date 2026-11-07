;; v128 block parameters: entry, both if arms, and a loop back-edge carrying a
;; vector. The AsmJIT value model stores a v128 as a buffer pointer, so a branch
;; must copy 16 bytes into the loop parameter buffer.
(module
  (func (export "block") (result i32)
    i32.const 42
    v128.const i32x4 1 2 3 4
    block (param i32 v128) (result i32)
      drop
    end)

  (func (export "if") (param i32) (result i32)
    v128.const i32x4 1 2 3 4
    local.get 0
    if (param v128) (result i32)
      drop
      i32.const 7
    else
      drop
      i32.const 8
    end)

  (func (export "loop") (result i32) (local $n i32) (local $v v128)
    i32.const 3
    v128.const i32x4 7 0 0 0
    loop $l (param i32 v128) (result i32)
      local.set $v
      local.set $n
      local.get $n
      i32.eqz
      if (result i32)
        local.get $v
        i32x4.extract_lane 0
      else
        local.get $n
        i32.const 1
        i32.sub
        local.get $v
        br $l
      end
    end))
