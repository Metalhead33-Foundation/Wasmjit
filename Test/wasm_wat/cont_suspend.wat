;; Stage-2 stack switching: the generator suspends once. The handler label
;; receives `[i32 (ref null cont)]` (the tag payload and the captured
;; continuation); run drops the continuation and returns the payload. Dropping
;; a suspended continuation cancels and joins its worker.
(module
  (type $ft (func (result i32)))
  (type $ct (cont $ft))
  (tag $yield (param i32) (result i32))
  (type $handler (func (result i32 (ref null cont))))

  (func $gen (type $ft)
    i32.const 7
    suspend $yield
    i32.const 1
    i32.add)

  (elem declare func $gen)

  (func (export "run") (type $ft)
    (block (type $handler)
      ref.func $gen
      cont.new $ct
      resume $ct (on $yield 0)
      return)
    drop))
