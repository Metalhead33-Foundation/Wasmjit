#ifndef TESTSTORE_HPP
#define TESTSTORE_HPP

#include "WasmBase/WasmStore.hpp"

// The store the harness parses and instantiates into.
//
// The engine has no process-wide store (docs/EMBEDDING_SURFACE.md §4.3
// option B): a `Runtime` owns one, and a test that drives the parser and the
// instantiator directly owns one too. Pass 2 keeps the harness on the legacy
// global store because the backend's string helpers still read it; pass 3 moves
// the harness onto its own store and deletes the global.
inline WASM::Store& testStore()
{
	return WASM::Store::global();
}

#endif // TESTSTORE_HPP
