// Public-header self-containment check (docs/EMBEDDING_SURFACE.md §6.1).
// This translation unit includes exactly one header and nothing else: if the
// header stops compiling on its own, the build fails here rather than in an
// embedder project.
#include <WasmBase/WasmNullInstantiator.hpp>
