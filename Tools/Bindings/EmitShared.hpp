#ifndef WASMJIT_BINDINGS_EMITSHARED_HPP
#define WASMJIT_BINDINGS_EMITSHARED_HPP

#include <nlohmann/json.hpp>

#include <cstddef>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

/*!
  \file EmitShared.hpp
  \brief Shared helpers for the binding emitters.

  The helpers here are tooling internals. They consume the ordered JSON view
  that `Schema::toJson()` produces; the public generator operates on the typed
  model in `BindingModel.hpp`.
*/
namespace WASM::Bindings::detail {

using Json = nlohmann::ordered_json; //!< Ordered JSON used at the emit boundary.

//! Makes a schema name safe for every target language.
std::string sanitize(std::string_view text);

//! Quotes a name for a C or Rust string literal.
std::string quoteName(std::string_view text, bool rust);

//! True when the value type is a reference.
bool isReference(const Json& type);

//! Returns the target-neutral opaque name for a reference type.
std::string refTypeName(const Json& type);

//! Returns the C spelling of a value type.
std::string cType(const Json& type);

//! Returns the stable opaque name of a WIT type.
std::string witOpaqueTypeName(std::string_view name);

//! True when a WIT type definition is an alias rather than a structured type.
bool witTypeIsAlias(const Json& definition);

//! Emits forward declarations for the named types of a WIT schema.
void emitWitOpaqueTypes(std::ostringstream& out, const Json& schema, bool cpp);

//! Collects opaque reference names from a schema fragment.
void collectRefs(const Json& node, std::set<std::string>& names);

//! Collects opaque reference names from a complete schema.
void collectSchemaRefs(const Json& schema, std::set<std::string>& names);

//! Collects the generated names of all non-alias WIT types.
void collectWitTypeNames(const Json& schema, std::set<std::string>& names);

//! Returns an import's Wasm module, falling back to `component`.
std::string importModule(const Json& entry);

//! Returns the generated C/C++ name of an import.
std::string importName(const Json& entry);

//! Flattens prose into one line for a generated comment.
std::string commentText(std::string text);

//! Returns an entry's optional description.
std::string functionDescription(const Json& entry);

//! Returns the display comment of an import.
std::string importComment(const Json& entry);

//! Returns the display comment of an export.
std::string exportComment(const Json& entry);

//! Returns an argument name, falling back to `argN`.
std::string argumentName(const Json& entry, std::size_t index);

//! Returns the Canonical ABI or structured function parameters.
const Json& signatureParams(const Json& entry);

//! Returns the Canonical ABI or structured function results.
const Json& signatureResults(const Json& entry);

//! Emits one C-style declaration.
void emitFunction(std::ostringstream& out, const Json& entry, const std::string& name,
		const char* side, const std::string& attribute = {});

//! Emits the C header body.
std::string emitCHeaderImpl(const Json& schema);

//! Returns the PascalCase spelling of a name.
std::string pascalName(std::string_view name);

//! Returns the Pascal-case reference type name.
std::string refPascal(const Json& type);

//! Returns the Rust spelling of a value type.
std::string rustType(const Json& type);

//! Emits Rust parameters for one entry.
void emitRustParams(std::ostringstream& out, const Json& entry, bool resultBuffer);

//! Emits a Rust return type.
void emitRustReturn(std::ostringstream& out, const Json& results);

//! Returns the TypeScript spelling of a value type.
std::string tsType(const Json& type);

//! Returns the AssemblyScript spelling of a value type.
std::string asType(const Json& type);

//! Returns the Zig spelling of a value type.
std::string zigType(const Json& type);

//! Returns the Kotlin spelling of a value type.
std::string kotlinType(const Json& type);

//! Returns the MoonBit spelling of a value type.
std::string moonbitType(const Json& type);

//! Returns the Go spelling of a value type.
std::string goType(const Json& type);

//! Returns the Dart FFI spelling of a value type.
std::string dartType(const Json& type);

//! Returns the Dart native spelling of a value type.
std::string dartNativeType(const Json& type);

//! Returns the Python ctypes spelling of a value type.
std::string pythonType(const Json& type);

//! Emits comments for non-function entries.
std::string nonFunctionNotes(const Json& schema);

//! Removes duplicate function entries from a JSON schema, keeping the first.
void dedupeJsonSchema(Json& schema);

/*!
 * \brief Iterates over function entries in one schema array.
 * \tparam Fn The callable type receiving `const Json&`.
 * \param schema The schema to inspect.
 * \param key `"imports"` or `"exports"`.
 * \param fn The callable to invoke.
 */
template <typename Fn>
void forEachFunction(const Json& schema, const char* key, Fn fn)
{
	for (const Json& entry : schema.at(key)) {
		if (entry.value("kind", "") != "function")
			continue;
		fn(entry);
	}
}

} // namespace WASM::Bindings::detail

#endif // WASMJIT_BINDINGS_EMITSHARED_HPP
