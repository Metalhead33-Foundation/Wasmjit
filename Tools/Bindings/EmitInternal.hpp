#ifndef WASMJIT_BINDINGS_EMITINTERNAL_HPP
#define WASMJIT_BINDINGS_EMITINTERNAL_HPP

#include <nlohmann/json.hpp>

/*!
  \file EmitInternal.hpp
  \brief Declarations shared by the emitter translation units and the dispatcher.
*/
namespace WASM::Bindings::detail {

//! Emits TypeScript declarations.
std::string emitTypeScriptImpl(const nlohmann::ordered_json& schema);

//! Emits AssemblyScript declarations.
std::string emitAssemblyScriptImpl(const nlohmann::ordered_json& schema);

//! Emits Zig declarations.
std::string emitZigImpl(const nlohmann::ordered_json& schema);

//! Emits Kotlin/Wasm declarations.
std::string emitKotlinImpl(const nlohmann::ordered_json& schema);

//! Emits MoonBit declarations.
std::string emitMoonBitImpl(const nlohmann::ordered_json& schema);

//! Emits Go Wasm declarations.
std::string emitGoImpl(const nlohmann::ordered_json& schema);

//! Emits Dart FFI declarations.
std::string emitDartImpl(const nlohmann::ordered_json& schema);

//! Emits Java declarations.
std::string emitJavaImpl(const nlohmann::ordered_json& schema);

//! Emits C# declarations.
std::string emitCSharpImpl(const nlohmann::ordered_json& schema);

//! Emits F# declarations.
std::string emitFSharpImpl(const nlohmann::ordered_json& schema);

//! Emits Python ctypes declarations.
std::string emitPythonImpl(const nlohmann::ordered_json& schema);

//! Emits Ruby/Fiddle declarations.
std::string emitRubyImpl(const nlohmann::ordered_json& schema);

//! Emits LuaJIT FFI declarations.
std::string emitLuaImpl(const nlohmann::ordered_json& schema);

//! Emits Swift declarations.
std::string emitSwiftImpl(const nlohmann::ordered_json& schema);

//! Emits D `extern(C)` declarations.
std::string emitDLangImpl(const nlohmann::ordered_json& schema);

//! Emits Nim declarations.
std::string emitNimImpl(const nlohmann::ordered_json& schema);

//! Emits Crystal `lib` bindings.
std::string emitCrystalImpl(const nlohmann::ordered_json& schema);

//! Emits Ada import/export bindings.
std::string emitAdaImpl(const nlohmann::ordered_json& schema);

//! Emits Pascal `cdecl` declarations.
std::string emitPascalImpl(const nlohmann::ordered_json& schema);

//! Emits Haskell `foreign import ccall` declarations.
std::string emitHaskellImpl(const nlohmann::ordered_json& schema);

//! Emits C++ `Callable` typedefs.
std::string emitCppTypedefsImpl(const nlohmann::ordered_json& schema);

//! Emits C++ cast/load helpers.
std::string emitCppSourceImpl(const nlohmann::ordered_json& schema);

//! Emits a compiled C++ adapter.
std::string emitCppAdapterImpl(const nlohmann::ordered_json& schema);

//! Emits a WIT package/world representation.
std::string emitWitImpl(const nlohmann::ordered_json& schema);

//! Emits a legacy WITX module representation.
std::string emitWitxImpl(const nlohmann::ordered_json& schema);

} // namespace WASM::Bindings::detail

#endif // WASMJIT_BINDINGS_EMITINTERNAL_HPP
