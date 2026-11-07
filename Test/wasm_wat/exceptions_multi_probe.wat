;; Item 2: a catch that carries more than one value, and catch_ref. These are the
;; two shapes AsmJIT's one-result structured blocks used to refuse; the block now
;; carries N results.
(module
  (tag $e2 (param i32 i64))
  (tag $e1 (param i32))
  (tag $e2i (param i32 i32))



  (func (export "block2direct") (result i32)
    block (result i32 i32)
      i32.const 11
      i32.const 22
    end
    i32.add)

  ;; A plain two-result block, no exceptions: isolates the multi-result block
  ;; machinery from the catch routing.
  (func (export "block2") (result i32) (local $a i32) (local $b i32)
    block (result i32 i32)
      i32.const 11
      i32.const 22
    end
    local.set $b
    local.set $a
    local.get $a
    local.get $b
    i32.add)

  ;; Two payload values, routed to a two-result block.
  (func (export "catch2") (result i64) (local $hi i64)
    block $h (result i32 i64)
      try_table (result i32 i64) (catch $e2 $h)
        i32.const 11
        i64.const 22
        throw $e2
      end
      unreachable
    end
    local.set $hi
    drop
    local.get $hi)


  ;; Two i32 payload values, routed to a two-i32 block.
  (func (export "catch2i32") (result i32) (local $a i32) (local $b i32)
    block $h (result i32 i32)
      try_table (result i32 i32) (catch $e2i $h)
        i32.const 11
        i32.const 22
        throw $e2i
      end
      unreachable
    end
    local.set $b
    local.set $a
    local.get $a
    local.get $b
    i32.add)

  ;; Payload plus exnref, routed to a two-result block.
  (func (export "catchref") (result i32) (local $x i32) (local $exn exnref)
    block $h (result i32 exnref)
      try_table (result i32) (catch_ref $e1 $h)
        i32.const 33
        throw $e1
      end
      unreachable
    end
    local.set $exn
    local.set $x
    local.get $x)
)
