;; Item 3: structured block/loop/if parameters (multi-value proposal).
;; A block parameter starts the body with values already on the stack; a loop
;; parameter is what a back-edge must carry; an if parameter is the initial
;; stack of both arms.
(module
  (type $b (func (param i32) (result i32)))

  (func (export "block") (param i32) (result i32)
    local.get 0
    block (type $b)
      i32.const 1
      i32.add
    end)

  (func (export "if") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    if (type $b)
      i32.const 1
      i32.add
    else
      i32.const 2
      i32.add
    end)

  (func (export "loop") (param i32) (result i32) (local i32)
    local.get 0
    loop (type $b)
      local.tee 1
      i32.eqz
      if (result i32)
        local.get 1
      else
        local.get 1
        i32.const 1
        i32.sub
        br 1
      end
    end))
