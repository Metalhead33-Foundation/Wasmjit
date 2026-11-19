#ifndef WASMJIT_BINDINGS_EMITOBJECTS_HPP
#define WASMJIT_BINDINGS_EMITOBJECTS_HPP

#include <nlohmann/json.hpp>

#include <sstream>
#include <string>
#include <string_view>

/*!
  \file EmitObjects.hpp
  \brief Emission of host-registered object descriptors (`wasmjit.binding/2`).

  The input is the validated `objects` view `Schema::toJson()` produces, so the
  offsets and sizes printed here are the ones `WASM::layoutHostType` derived,
  never a number a descriptor merely asserted
  (docs/proposals/SYNTHETIC_OBJECTS.md A.14.5). Tooling only: this header is
  never included from `WasmBase/`.
*/
namespace WASM::Bindings::detail {

//! Returns the lower-case snake_case identifier of one descriptor name.
/*!
  \brief Normalizes a descriptor name for C and Rust identifiers.
  \param name The descriptor name, for example `demo.Widget`.
  \return A lower-case identifier whose non-alphanumeric characters are `_`.
*/
std::string objectIdentifier(std::string_view name);

//! Returns the C/Rust type name of one `(name, version)` descriptor.
/*!
  \brief Builds the versioned type name of one object descriptor.
  \param object The validated object descriptor.
  \return `wasmjit_object_<identifier>_v<version>`.
*/
std::string objectTypeName(const nlohmann::ordered_json& object);

//! Emits the C declarations for a schema's objects.
/*!
  \brief Emits one C block: constants, opaque types, method typedefs and the
         walkable descriptor tables.
  \param out The output stream.
  \param schema The validated schema view; nothing is emitted when it has no objects.
*/
void emitCObjects(std::ostringstream& out, const nlohmann::ordered_json& schema);

//! Emits the Rust declarations for a schema's objects.
/*!
  \brief Emits one Rust block: constants, opaque types, method aliases and the
         descriptor statics.
  \param out The output stream.
  \param schema The validated schema view; nothing is emitted when it has no objects.
*/
void emitRustObjects(std::ostringstream& out, const nlohmann::ordered_json& schema);

//! Emits the WIT resources for a schema's objects.
/*!
  \brief Emits one `resource` declaration per object, with field accessors and
         methods that WIT can express; the rest is noted and omitted.
  \param out The output stream.
  \param schema The validated schema view; nothing is emitted when it has no objects.
*/
void emitWitObjects(std::ostringstream& out, const nlohmann::ordered_json& schema);

} // namespace WASM::Bindings::detail

#endif // WASMJIT_BINDINGS_EMITOBJECTS_HPP
