#ifndef WASMJIT_BINDINGS_BINDINGMODEL_HPP
#define WASMJIT_BINDINGS_BINDINGMODEL_HPP

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/*!
  \file BindingModel.hpp
  \brief The typed binding-schema model used by the generator internals.

  JSON is an interchange format, not the working representation. The CLI and
  the optional C++ front-end parse JSON into these structures, operate on
  enums and fields, and format a \c Schema back to JSON only at an output
  boundary. Tooling only: this header is never included from `WasmBase/`.
*/
namespace WASM::Bindings {

/*! \brief The direction a generated adapter faces. */
enum class Direction {
	GuestToHost, //!< A Wasm guest calls a host function (host-side imports).
	HostToGuest, //!< A native host calls a module/component export.
	GuestToGuest, //!< A consumer imports the other module/component's exports.
	Complete //!< Save all imports and exports; no direction projection.
};

/*! \brief The kind of an extern entry in a binding schema. */
enum class EntryKind {
	Function, //!< A core or component function.
	Table, //!< A core table.
	Memory, //!< A core memory.
	Global, //!< A core global.
	Tag, //!< A core tag.
	Type, //!< A type export/import.
	Value, //!< A component value.
	Instance, //!< A component instance.
	Component, //!< A nested component.
	CoreModule, //!< A core module.
	Unknown //!< A kind this tool does not recognise.
};

/*! \brief The scalar or reference kind of one core ABI value. */
enum class ValueKind {
	I32, //!< A 32-bit integer.
	I64, //!< A 64-bit integer.
	F32, //!< A 32-bit float.
	F64, //!< A 64-bit float.
	V128, //!< A 128-bit vector.
	I8, //!< A packed 8-bit signed integer.
	I16, //!< A packed 16-bit signed integer.
	U8, //!< A packed 8-bit unsigned integer.
	U16, //!< A packed 16-bit unsigned integer.
	U32, //!< A 32-bit unsigned integer.
	U64, //!< A 64-bit unsigned integer.
	S8, //!< A WIT signed 8-bit integer.
	S16, //!< A WIT signed 16-bit integer.
	S32, //!< A WIT signed 32-bit integer.
	S64, //!< A WIT signed 64-bit integer.
	Bool, //!< A component boolean.
	Char, //!< A component character.
	String, //!< A component string.
	ErrorContext, //!< A component error-context.
	Ref, //!< A GC/reference value.
	StringRef, //!< A `stringref` value.
	StringViewWtf8, //!< A `stringview_wtf8` value.
	StringViewWtf16, //!< A `stringview_wtf16` value.
	StringViewIter, //!< A `stringview_iter` value.
	Void, //!< No value.
	Unknown //!< A kind this tool does not recognise.
};

/*! \brief The heap kind of a WebAssembly reference value. */
enum class HeapKind {
	Func, //!< `funcref`.
	Extern, //!< `externref`.
	Any, //!< `anyref`.
	Eq, //!< `eqref`.
	I31, //!< `i31ref`.
	Struct, //!< `structref`.
	Array, //!< `arrayref`.
	Exn, //!< `exnref`.
	Cont, //!< `contref`.
	NoCont, //!< `nocontref`.
	None, //!< `nullref`.
	NoFunc, //!< `nullfuncref`.
	NoExtern, //!< `nullexternref`.
	NoExn, //!< `nullexnref`.
	Defined, //!< A defined type, identified by `typeKind`.
	Rec, //!< A recursion-group member.
	Unknown //!< A heap kind this tool does not recognise.
};

/*!
 * \brief One core ABI value type, parsed from a schema `type` object.
 */
struct ValueType {
	ValueKind kind = ValueKind::Unknown; //!< The value kind.
	bool packed = false; //!< True for a packed array/field element.
	bool nullable = true; //!< True for `ref null`.
	bool shared = false; //!< True for a shared reference.
	bool exact = false; //!< True for an exact reference type.
	HeapKind heap = HeapKind::Unknown; //!< The heap kind of a reference.
	std::string typeKind; //!< The defined type kind (`struct`, `array`, ...).
	std::string name; //!< An optional interface/type name.
	std::uint32_t typeId = 0; //!< Tooling-only defined-type id.
	std::uint32_t memberIndex = 0; //!< Recursion-group member index.
	bool hasTypeId = false; //!< True when `typeId` is meaningful.

	/*!
	 * \brief Parses one value type from its JSON spelling.
	 * \param json The schema value.
	 * \return The typed value.
	 */
	static ValueType fromJson(const nlohmann::ordered_json& json);

	/*!
	 * \brief Formats the value type for schema interchange.
	 * \return The ordered JSON object.
	 */
	nlohmann::ordered_json toJson() const;
};

/*!
 * \brief A function parameter/result list, preferring the Canonical ABI form.
 */
struct FunctionType {
	std::vector<ValueType> params; //!< The function parameters.
	std::vector<ValueType> results; //!< The function results.
	bool fromAbi = false; //!< True when the lists came from `abi`.
	bool indirectParams = false; //!< True for indirect Canonical ABI params.
	bool indirectResults = false; //!< True for indirect Canonical ABI results.
};

/*!
 * \brief One import or export entry in a binding schema.
 */
struct Entry {
	EntryKind kind = EntryKind::Unknown; //!< The entry kind.
	bool import = false; //!< True for an import entry.
	std::string module; //!< The Wasm import module, for imports.
	std::string name; //!< The function or field name.
	std::string description; //!< Optional prose from a front-end.
	std::vector<std::string> argNames; //!< Optional parameter names.
	FunctionType function; //!< The parsed function signature.
	nlohmann::ordered_json raw; //!< The original entry, for open-ended fields.

	/*!
	 * \brief Parses one schema entry.
	 * \param json The entry object.
	 * \param import True when the entry belongs to the imports array.
	 * \return The typed entry.
	 */
	static Entry fromJson(const nlohmann::ordered_json& json, bool import);

	/*!
	 * \brief Formats the entry for schema interchange.
	 * \return The ordered JSON object.
	 */
	nlohmann::ordered_json toJson() const;
};

/*!
 * \brief A complete binding schema, with typed function entries.
 *
 * Component value/type payloads remain JSON because the schema intentionally
 * carries open-ended component structures; every value the emitters call is
 * available through \c Entry and \c ValueType.
 */
struct Schema {
	std::string id = "wasmjit.binding/1"; //!< The schema identifier.
	std::string module = "module"; //!< The module/component name.
	std::string frontend; //!< The optional producing front-end name.
	Direction direction = Direction::GuestToHost; //!< The schema direction.
	std::vector<Entry> imports; //!< Import entries.
	std::vector<Entry> exports; //!< Export entries.
	nlohmann::ordered_json types = nlohmann::ordered_json::array(); //!< Type definitions.
	nlohmann::ordered_json extras = nlohmann::ordered_json::object(); //!< Other envelope fields.

	/*!
	 * \brief Parses a schema from JSON.
	 * \param json The schema object.
	 * \return The typed schema.
	 * \throws std::runtime_error when the identifier or direction is invalid.
	 */
	static Schema fromJson(const nlohmann::ordered_json& json);

	/*!
	 * \brief Formats the schema for interchange.
	 * \return The ordered JSON object.
	 */
	nlohmann::ordered_json toJson() const;

	/*! \brief Removes duplicate function entries, keeping the first. */
	void dedupe();
};

/*!
 * \brief Returns the CLI/interchange spelling of a direction.
 * \param direction The direction to name.
 * \return `"g2h"`, `"h2g"`, `"g2g"` or `"all"`.
 */
const char* directionName(Direction direction) noexcept;

/*!
 * \brief Parses a direction spelling.
 * \param name The spelling to parse.
 * \param direction Receives the parsed direction.
 * \return true when the name is recognised.
 */
bool parseDirection(std::string_view name, Direction& direction) noexcept;

/*!
 * \brief Returns the schema spelling of an entry kind.
 * \param kind The kind to name.
 * \return The lower-case schema spelling.
 */
const char* entryKindName(EntryKind kind) noexcept;

/*!
 * \brief Parses an entry kind spelling.
 * \param name The spelling to parse.
 * \return The entry kind, or \c EntryKind::Unknown.
 */
EntryKind entryKindFromName(std::string_view name) noexcept;

/*!
 * \brief Returns the schema spelling of a scalar value kind.
 * \param kind The kind to name.
 * \return The lower-case schema spelling.
 */
const char* valueKindName(ValueKind kind) noexcept;

/*!
 * \brief Parses a scalar value kind spelling.
 * \param name The spelling to parse.
 * \return The value kind, or \c ValueKind::Unknown.
 */
ValueKind valueKindFromName(std::string_view name) noexcept;

/*!
 * \brief Returns the schema spelling of a reference heap kind.
 * \param kind The kind to name.
 * \return The lower-case schema spelling.
 */
const char* heapKindName(HeapKind kind) noexcept;

/*!
 * \brief Parses a reference heap kind spelling.
 * \param name The spelling to parse.
 * \return The heap kind, or \c HeapKind::Unknown.
 */
HeapKind heapKindFromName(std::string_view name) noexcept;

} // namespace WASM::Bindings

#endif // WASMJIT_BINDINGS_BINDINGMODEL_HPP
