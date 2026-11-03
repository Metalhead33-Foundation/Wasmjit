;; CM-20/CM-27: the thread built-ins are gated behind ComponentFeature::Threads.
;; The exported `thread-index` function lifts `canon thread.index` so the
;; embedder-supplied executor's answer can be observed end to end.
(component
  (canon thread.index (core func $thread.index))
  (canon thread.available_parallelism (core func $thread.parallelism))
  (type $ft (func (result u32)))
  (canon lift (core func $thread.index) (func $f (type $ft)))
  (canon lift (core func $thread.parallelism) (func $p (type $ft)))
  (export "thread-index" (func $f))
  (export "thread-parallelism" (func $p))
)
