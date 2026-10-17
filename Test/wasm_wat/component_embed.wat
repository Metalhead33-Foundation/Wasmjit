;; A component embedding one core module. Exercises the Component Model
;; parser's core:module section (CM-1a): the nested body has no preamble and is
;; decoded with the regular core-module section parser.
(component
  (core module $m
    (func (export "answer") (result i32)
      (i32.const 42)
    )
  )
)