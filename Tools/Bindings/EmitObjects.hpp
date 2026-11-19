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
  (docs/proposals/SYNTHETIC_OBJECTS.md A.14.5). A `properties` entry is emitted as
  an accessor pair instead: it has no offset to print, and the names it carries
  are the export names a host binds a trampoline to (§4.7 of the same note).
  Tooling only: this header is never included from `WasmBase/`.
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

//! Emits the C++ RAII wrappers over the handle surface (`SO-3`/`SO-4`).
/*!
  \brief Emits one `#ifdef __cplusplus` class block per object.
  \param out The output stream.
  \param schema The validated schema view; nothing is emitted when it has no objects.

  The block is written *after* the header's `extern "C"` section closes and
  before the header guard ends, because a class cannot have C linkage. Each class
  owns one slot: it is move-only, its destructor calls the object's `_release`
  entry point, and `reset()`/`release()` spell the two ways to give the slot up.
*/
void emitCppObjects(std::ostringstream& out,
		const nlohmann::ordered_json& schema);

//! Emits one Kotlin/Wasm wrapper class per object: the façade that holds the handle.
/*!
  \brief Emits the garbage-collected-language façade (`SO-4`).
  \param out The output stream.
  \param schema The validated schema view; nothing is emitted when it has no objects.

  The wrapper owns nothing the engine counts: it holds the `i64` slot and calls
  the host's release once, so `close()`/`use { }` is the lifetime policy the
  component model spells `resource.drop` (docs/proposals/SYNTHETIC_OBJECTS.md
  A.14.4).
*/
void emitKotlinObjects(std::ostringstream& out,
		const nlohmann::ordered_json& schema);

} // namespace WASM::Bindings::detail

#endif // WASMJIT_BINDINGS_EMITOBJECTS_HPP
