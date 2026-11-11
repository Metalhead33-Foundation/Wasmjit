;; `GAP-5`: a native import that *returns* a reference. A host C function keeps
;; its own count of the value it hands back, so the guest must clone the result
;; before its operand stack owns it; taking the count over would release the
;; host's and free an object the host still holds (docs/WRITING_A_BACKEND.md §6,
;; "Reference results: a Wasm callee transfers, a native import borrows").
;;
;; `make` gives the test an object to hold; `hand_back_and_drop` consumes the
;; import's result and discards it, which is exactly the shape that over-released
;; before the rule was implemented on all three backends.
(module
  (type $pair (struct (field i32)))
  (import "env" "hand_back" (func $hand_back (result (ref null $pair))))

  (func (export "make") (result (ref $pair))
    (struct.new $pair (i32.const 7)))

  (func (export "hand_back_and_drop")
    (drop (call $hand_back)))

  (func (export "hand_back_and_return") (result (ref null $pair))
    (call $hand_back)))
