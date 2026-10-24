;; `cont.bind` partially applies the first of two i32 parameters; `resume`
;; supplies the remaining one. The runtime keeps the bound prefix on the
;; continuation and passes both to the target.
(module
  (type $ft0 (func (param i32 i32) (result i32)))
  (type $ct0 (cont $ft0))
  (type $ft1 (func (param i32) (result i32)))
  (type $ct1 (cont $ft1))

  (func $gen (type $ft0)
    local.get 0
    local.get 1
    i32.add)

  (elem declare func $gen)

  (func (export "run") (param i32) (result i32)
    (local $c (ref null $ct1))
    i32.const 10
    ref.func $gen
    cont.new $ct0
    cont.bind $ct0 $ct1
    local.set $c
    local.get 0
    local.get $c
    resume $ct1))
