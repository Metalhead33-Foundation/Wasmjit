#ifndef TESTSTORE_HPP
#define TESTSTORE_HPP

#include "WasmBase/WasmStore.hpp"

// The store the harness parses and instantiates into.
//
// Pass 1 of ES-2 option B threads an explicit store through the parser; the
// instantiator still reaches `Store::current()` until pass 2, so for now this
// returns the same store that fallback yields. Pass 2 replaces the body with a
// harness-owned store and deletes the fallback.
inline WASM::Store& testStore()
{
	return WASM::Store::global();
}

#endif // TESTSTORE_HPP
