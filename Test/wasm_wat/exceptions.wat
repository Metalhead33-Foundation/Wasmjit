;; docs/proposals/EXCEPTIONS.md EX-3/EX-5: a caught `throw` (with its payload)
;; and an uncaught one, which leaves the exception pending for the embedder.
(module
  (tag $e (param i32))

  ;; Catches the payload and returns payload + 1.
  (func (export "catch") (param i32) (result i32)
    (block $h (result i32)
      (try_table (catch $e $h)
        (throw $e (local.get 0))
      )
    )
    (i32.add (i32.const 1)))

  ;; No handler: the exception propagates out as a pending exception.
  (func (export "throw-uncaught") (param i32)
    (throw $e (local.get 0)))
)
