;; Wide suspension payload: the tag carries an i32 and an externref and the
;; resumed continuation expects two i32 results. The handler label receives the
;; full payload plus the captured continuation.
(module
  (type $ft (func (param i32 externref) (result i32)))
  (type $ct (cont $ft))
  (type $ftr (func (param i32 i32) (result i32)))
  (type $ctr (cont $ftr))
  (type $handler (func (result i32 externref (ref null cont))))
  (tag $wide (param i32 externref) (result i32 i32))

  (func $gen (type $ft)
    i32.const 7
    local.get 1
    suspend $wide
    i32.add)

  (elem declare func $gen)

  (func (export "run") (result i32)
    (local $c (ref null cont))
    (local $r externref)
    (block $on_wide (type $handler)
      i32.const 0
      ref.null extern
      ref.func $gen
      cont.new $ct
      resume $ct (on $wide 0)
      return)
    local.set $c
    local.set $r
    drop
    i32.const 20
    i32.const 22
    local.get $c
    resume $ctr))
