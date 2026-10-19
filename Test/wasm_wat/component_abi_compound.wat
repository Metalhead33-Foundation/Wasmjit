;; Canonical ABI flat/layout fixture for enum, flags, option, result, variant,
;; map, stream and future.
(component
  (type $enum (enum "a" "b"))
  (type $flags (flags "a" "b"))
  (type $opt (option u32))
  (type $res (result u32 (error u32)))
  (type $variant (variant (case "a") (case "b" u32)))
  (type $map (map string u32))
  (type $stream (stream u32))
  (type $future (future u32))
  (type $fn (func
    (param "e" $enum) (param "f" $flags) (param "o" $opt)
    (param "r" $res) (param "v" $variant) (param "m" $map)
    (param "s" $stream) (param "x" $future)))
)
