#ifndef TESTSTORE_HPP
#define TESTSTORE_HPP

#include "WasmBase/WasmStore.hpp"

// The store the harness parses and instantiates into.
//
// The engine has no process-wide store (docs/EMBEDDING_SURFACE.md §4.3
// option B): a `Runtime` owns one, and a test that drives the parser and the
// instantiator directly owns one too. One inline function gives the whole test
// binary a single store.
inline WASM::Store& testStore()
{
	static WASM::Store store;
	return store;
}

#endif // TESTSTORE_HPP
