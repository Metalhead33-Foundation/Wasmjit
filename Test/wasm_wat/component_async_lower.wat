;; CM19: async lower. The host import is async-typed; `canon lower ... async`
;; gives it the core signature (param i32 i32) -> (result i32): one scalar
;; parameter, a result pointer, and the packed subtask state. The caller stores
;; the returned u32 at address 0 and loads it back.
(component
  (core module $Mem (memory (export "mem") 1))
  (core instance $memInst (instantiate $Mem))
  (alias core export $memInst "mem" (core memory $mem))
  (import "host" (func $host async (param "x" u32) (result u32)))
  (canon lower (func $host) async (memory $mem)
    (core func $lowered))
  (core module $Caller
    (import "" "f" (func $f (param i32 i32) (result i32)))
    (import "" "mem" (memory 1))
    (func (export "call") (param i32) (result i32)
      (drop (call $f (local.get 0) (i32.const 0)))
      (i32.load (i32.const 0))
    )
  )
  (core instance $callerInst (instantiate $Caller
    (with "" (instance
      (export "f" (func $lowered))
      (export "mem" (memory $mem))))))
  (alias core export $callerInst "call" (core func $call))
)
