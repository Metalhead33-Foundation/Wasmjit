#ifndef TESTSTORE_HPP
#define TESTSTORE_HPP

#include "WasmBase/WasmStore.hpp"

/*!
  \file TestStore.hpp
  \brief The single store the harness parses and instantiates into.

  The engine has no process-wide store: a Runtime owns one, and a test that
  drives the parser and instantiator directly owns one too. This inline accessor
  gives the test binary one shared store.
  This header is internal: it is not part of the embedding surface.
*/

/*!
 * \brief Returns the shared WebAssembly store used by the test harness.
 *
 * The store is constructed on first use and reused for the lifetime of the
 * test binary. Tests using it therefore share store-owned state.
 *
 * \return Reference to the process-wide test store.
 */
inline WASM::Store& testStore()
{
	static WASM::Store store;
	return store;
}

#endif // TESTSTORE_HPP
