#ifndef TESTBACKEND_HPP
#define TESTBACKEND_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "WasmBase/WasmBackendSession.hpp"
#include "WasmBase/WasmModuleInstance.hpp"
#include "WasmBase/WasmTrap.hpp"

/*!
  \file TestBackend.hpp
  \brief The harness alias of the backend session facade.

  This header is internal: it is not part of the embedding surface.
*/

namespace TestBackend {

//! The backend façade is a product type (`WASM::BackendSession`, AAS8); the
//! harness aliases it so a test body never depends on the harness being the
//! place where the interface was first written.
using FieldLayout = WASM::BackendFieldLayout;
//! The aggregate half of the same product type (`WASM::BackendAggregateLayout`).
using AggregateLayout = WASM::BackendAggregateLayout;
//! A vectorised lane-wise operation, as the session seam describes it.
using LaneWiseOp = WASM::BackendLaneWiseOp;
//! The session type itself: what `create()` returns.
using Session = WASM::BackendSession;

//! Selects the backend named by WASMJIT_BACKEND (empty/`libjit` → the compiled
//! backend). The selector lives here, not in the core: `WasmBase/` must not know
//! the name of a code generator (docs/LAYERING.md §2).
std::unique_ptr<Session> create();

} // namespace TestBackend

#endif // TESTBACKEND_HPP
