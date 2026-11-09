;; Historical name: this was the AsmJIT/SLJIT "unsupported" probe. Both now
;; support v128 block results (BLOCKRESV128), so it is a small positive fixture
;; for the SLJIT block-result path. The durable AsmJIT refusal probe is
;; asmjit_brtable_multi_unsupported.wat.
(module
  (func (export "already_compiled") (result i32)
    i32.const 42)
  (func (export "unsupported") (result i32)
    block (result v128)
      v128.const i32x4 0 0 0 0
    end
    drop
    i32.const 1))
