;; Multiple handlers and a loop-labelled handler. `multi_b`/`multi_a` prove
;; dispatch over more than one `(on $tag $label)` clause; `loop_handler` proves
;; that a loop label is a valid handler target and receives the tag payload plus
;; the captured continuation as its branch parameters.
(module
  (type $ft (func (result i32)))
  (type $ct (cont $ft))
  (type $handler (func (result i32 (ref null cont))))
  (tag $a (param i32) (result i32))
  (tag $b (param i32) (result i32))

  (func $gen_b (type $ft)
    i32.const 11
    suspend $b
    i32.const 1
    i32.add)

  (func $gen_a (type $ft)
    i32.const 22
    suspend $a
    i32.const 1
    i32.add)

  (func $gen_loop (type $ft)
    i32.const 7
    suspend $b
    i32.const 1
    i32.add)

  (elem declare func $gen_a $gen_b $gen_loop)

  (func (export "multi_b") (result i32)
    (block $on_a (type $handler)
      (block $on_b (type $handler)
        ref.func $gen_b
        cont.new $ct
        resume $ct (on $a 1) (on $b 0)
        return)
      drop
      drop
      i32.const 100
      return)
    drop
    drop
    i32.const 200)

  (func (export "multi_a") (result i32)
    (block $on_a (type $handler)
      (block $on_b (type $handler)
        ref.func $gen_a
        cont.new $ct
        resume $ct (on $a 1) (on $b 0)
        return)
      drop
      drop
      i32.const 100
      return)
    drop
    drop
    i32.const 200)

  (func (export "loop_handler") (result i32)
    (local $handled i32)
    (local $payload i32)
    (local $captured (ref null cont))
    (block $exit (result i32)
      i32.const 0
      ref.null cont
      loop $on (param i32 (ref null cont)) (result i32)
        local.set $captured
        local.set $payload
        local.get $handled
        if (result i32)
          local.get $payload
          i32.const 1
          i32.add
        else
          i32.const 1
          local.set $handled
          ref.func $gen_loop
          cont.new $ct
          resume $ct (on $b 1)
          br $exit
        end
      end))
)
