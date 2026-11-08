;; A br_table whose cases target a two-result block and the function label.
;; The four-parameter helper forces the module into the frame ABI.
(module
  (func $force (param i32 i32 i32 i32) (result i32)
    local.get 0)

  (func (export "run") (param i32) (result i32 i32)
    block (result i32 i32)
      i32.const 50
      i32.const 51
      local.get 0
      br_table 0 1 0
      i32.const 51
      i32.const -3
    end
    i32.add
    i32.const 52))
