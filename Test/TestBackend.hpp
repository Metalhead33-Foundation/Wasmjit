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
using AggregateLayout = WASM::BackendAggregateLayout;
using LaneWiseOp = WASM::BackendLaneWiseOp;
using Session = WASM::BackendSession;

//! Selects the backend named by WASMJIT_BACKEND (empty/`libjit` → the compiled
//! backend). The selector lives here, not in the core: `WasmBase/` must not know
//! the name of a code generator (docs/LAYERING.md §2).
std::unique_ptr<Session> create();

} // namespace TestBackend

#endif // TESTBACKEND_HPP
