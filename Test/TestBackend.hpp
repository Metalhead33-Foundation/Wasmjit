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

//! Backend field layout descriptor, aliased from the backend-independent API.
using FieldLayout = WASM::BackendFieldLayout;
//! Aggregate layout descriptor used by the backend session interface.
using AggregateLayout = WASM::BackendAggregateLayout;
//! Describes a backend-provided vectorised lane-wise operation.
using LaneWiseOp = WASM::BackendLaneWiseOp;
//! Backend session interface returned by create().
using Session = WASM::BackendSession;

/*!
 * \brief Creates the backend selected for the test harness.
 *
 * The WASMJIT_BACKEND environment variable selects the backend; an empty
 * value or `libjit` selects the compiled backend. The returned session is
 * owned by the caller.
 *
 * \return A newly created backend session, or nullptr if the selected backend
 *         cannot be created.
 */
std::unique_ptr<Session> create();

} // namespace TestBackend

#endif // TESTBACKEND_HPP
