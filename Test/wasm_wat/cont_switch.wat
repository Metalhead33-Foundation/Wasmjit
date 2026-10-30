;; Symmetric switching: `task` switches to `target`, passing its own
;; continuation; `target` switches back, passing its own. The second switch's
;; return value is the peer continuation, which each side drops. The outer
;; `resume` carries an `(on $yield switch)` clause, so the handler-clause decoder
;; and validator see the symmetric form as well as the direct handoff.
(module
  (rec
    (type $ft (func (param (ref null $ct)) (result i32)))
    (type $ct (cont $ft)))
  (tag $yield (result i32))

  (func $target (type $ft) (param $peer (ref null $ct)) (result i32)
    local.get $peer
    switch $ct $yield
    drop
    i32.const 40)

  (func $task (type $ft) (param $next (ref null $ct)) (result i32)
    (local $peer (ref null $ct))
    local.get $next
    switch $ct $yield
    local.set $peer
    local.get $peer
    local.get $peer
    resume $ct
    drop
    i32.const 7)

  (elem declare func $target $task)

  (func (export "run") (result i32)
    ref.func $target
    cont.new $ct
    ref.func $task
    cont.new $ct
    resume $ct (on $yield switch)))
