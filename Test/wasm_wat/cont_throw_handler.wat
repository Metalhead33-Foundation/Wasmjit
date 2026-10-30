;; `resume_throw` with a handler clause. The generator catches the injected
;; `$abort` inside `try_table`, then suspends again on `$yield`. The
;; `(on $yield 1)` clause is the path under test: it receives the second
;; suspension's payload and captured continuation.
(module
  (type $ft (func (result i32)))
  (type $ct (cont $ft))
  (tag $yield (param i32) (result i32))
  (tag $abort (param i32))

  (func $gen (type $ft)
    (block $caught (result i32)
      (try_table (catch $abort $caught)
        i32.const 7
        suspend $yield
        drop
        i32.const 1))
    drop
    i32.const 5
    suspend $yield
    drop
    i32.const 2)

  (elem declare func $gen)

  (func (export "run") (result i32)
    (local $c (ref null $ct))
    (block $on_first (result i32 (ref null cont))
      ref.func $gen
      cont.new $ct
      resume $ct (on $yield 0)
      return)
    local.set $c
    drop
    (block $on_second (result i32 (ref null cont))
      i32.const 9
      local.get $c
      resume_throw $ct $abort (on $yield 0)
      return)
    drop))
