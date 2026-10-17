;; Sign-extension operators (proposal sign-extension-ops, Phase 5 / Wasm 2.0).
;; Each export widens its argument and sign-extends the low 8/16/32 bits to the
;; register's full width. Exercised by the manual tests in Test/main.cpp.
(module
  (func (export "i32_extend8_s") (param i32) (result i32)
    local.get 0
    i32.extend8_s)

  (func (export "i32_extend16_s") (param i32) (result i32)
    local.get 0
    i32.extend16_s)

  (func (export "i64_extend8_s") (param i64) (result i64)
    local.get 0
    i64.extend8_s)

  (func (export "i64_extend16_s") (param i64) (result i64)
    local.get 0
    i64.extend16_s)

  (func (export "i64_extend32_s") (param i64) (result i64)
    local.get 0
    i64.extend32_s))
