#ifndef WASMJIT_BINDINGS_EMITINTERNAL_HPP
#define WASMJIT_BINDINGS_EMITINTERNAL_HPP

#include <nlohmann/json.hpp>

#include "BindingModel.hpp"

/*!
  \file EmitInternal.hpp
  \brief Declarations shared by the emitter translation units and the dispatcher.

  The emitters take two views of one schema: the **typed** model
  (`BindingModel.hpp`) for everything that is a structure, an enum or a number, and
  an ordered-JSON view only for the open-ended component payloads a schema
  deliberately carries. `typed` is never re-derived from `schema` inside an
  emitter — it is the same schema the caller parsed.
*/
namespace WASM::Bindings::detail {

//! Emits the Rust module body from both views.
/*!
  \brief Emits exports, imports and the object block.
  \param schema The ordered-JSON view, used for the open-ended entries.
  \param typed The typed schema, used for every object descriptor.
  \return The Rust source text.
*/
std::string emitRustModuleImpl(const nlohmann::ordered_json& schema,
		const Schema& typed);

//! Emits TypeScript declarations.
std::string emitTypeScriptImpl(const nlohmann::ordered_json& schema,
		const Schema& typed);

//! Emits AssemblyScript declarations.
std::string emitAssemblyScriptImpl(const nlohmann::ordered_json& schema,
		const Schema& typed);

//! Emits Zig declarations.
std::string emitZigImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits Kotlin/Wasm declarations.
std::string emitKotlinImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits MoonBit declarations.
std::string emitMoonBitImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits Go Wasm declarations.
std::string emitGoImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits Dart FFI declarations.
std::string emitDartImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits Java declarations.
std::string emitJavaImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits C# declarations.
std::string emitCSharpImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits F# declarations.
std::string emitFSharpImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits Python ctypes declarations.
std::string emitPythonImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits Ruby/Fiddle declarations.
std::string emitRubyImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits LuaJIT FFI declarations.
std::string emitLuaImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits Swift declarations.
std::string emitSwiftImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits D `extern(C)` declarations.
std::string emitDLangImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits Nim declarations.
std::string emitNimImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits Crystal `lib` bindings.
std::string emitCrystalImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits Ada import/export bindings.
std::string emitAdaImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits Pascal `cdecl` declarations.
std::string emitPascalImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits Haskell `foreign import ccall` declarations.
std::string emitHaskellImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits C++ `Callable` typedefs.
std::string emitCppTypedefsImpl(const nlohmann::ordered_json& schema);

//! Emits C++ cast/load helpers.
std::string emitCppSourceImpl(const nlohmann::ordered_json& schema);

//! Emits a compiled C++ adapter.
std::string emitCppAdapterImpl(const nlohmann::ordered_json& schema);

//! Emits a WIT package/world representation.
std::string emitWitImpl(const nlohmann::ordered_json& schema, const Schema& typed);

//! Emits a legacy WITX module representation.
std::string emitWitxImpl(const nlohmann::ordered_json& schema);

} // namespace WASM::Bindings::detail

#endif // WASMJIT_BINDINGS_EMITINTERNAL_HPP
