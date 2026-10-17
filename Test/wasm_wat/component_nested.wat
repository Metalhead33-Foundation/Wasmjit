;; A component embedding another component, which in turn embeds a core
;; module. Exercises recursive component-section parsing (CM-1a).
(component
  (component
    (core module)
  )
)