#ifndef WASMCOMPONENTVALUE_HPP
#define WASMCOMPONENTVALUE_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentValue.hpp
  \brief Host-facing Component Model values, functions and import bags.

  This header is part of the embedder surface: every declaration in it is
  exported (docs/EMBEDDING_SURFACE.md ES-4). The types here are the stable
  view an embedder uses to pass arguments/results and to describe host imports;
  the component AST and the runtime implementation stay internal.
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmTypeIdentity.hpp"
#include "WasmValue.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace WASM {

class LoadedComponent;
class Module;
class ComponentRuntime;

/** \brief The VM context of the current component host callback, or null.
 *
 * A host function invoked from component-embedded core code can use this to
 * recover `VMContext::hostData` and the caller's agent state. It is set for the
 * duration of a `canon lower` host call, including deferred work queued on the
 * component executor; direct host calls to an exported component function have
 * no core caller and therefore return null.
 */
const VMContext* currentComponentHostContext() noexcept;

/** \brief Interned string identity used by canonical component records. */
using StringId = uint32_t;

/** \brief Primitive Component Model value type. */
enum class PrimitiveValueType : uint8_t {
	Bool = 0x7F, //!< `bool`.
	S8 = 0x7E, //!< `s8`.
	U8 = 0x7D, //!< `u8`.
	S16 = 0x7C, //!< `s16`.
	U16 = 0x7B, //!< `u16`.
	S32 = 0x7A, //!< `s32`.
	U32 = 0x79, //!< `u32`.
	S64 = 0x78, //!< `s64`.
	U64 = 0x77, //!< `u64`.
	F32 = 0x76, //!< `float32`.
	F64 = 0x75, //!< `float64`.
	Char = 0x74, //!< `char`.
	String = 0x73, //!< `string`.
	ErrorContext = 0x64, //!< `error-context`.
	Invalid = 0xFF, //!< The "not a primitive" marker.
};

/** \brief Component value type as a parser-local index, a primitive, or a canonical TypeId. */
struct ComponentValueType {
	//! Which member of the triple below is meaningful.
	enum class Kind : uint8_t {
		TypeIndex, //!< A parser-local index into the component type index space.
		Primitive, //!< A `PrimitiveValueType`.
		TypeId //!< A canonical `TypeId`.
	};
	//! Which member of the triple is meaningful.
	Kind kind = Kind::Primitive;
	//! The primitive value type, when `kind` is `Primitive`.
	PrimitiveValueType primitive = PrimitiveValueType::Invalid;
	//! TypeIndex for parser syntax, TypeId.value for canonical records.
	uint32_t typeIndex = 0;
};

//! \brief Core scalar type used by the flattened Canonical ABI signature.
/** \brief Core scalar type in a flattened Canonical ABI signature. */
enum class AbiCoreType : uint8_t {
	I32, //!< A 32-bit integer cell.
	I64, //!< A 64-bit integer cell.
	F32, //!< A 32-bit float cell.
	F64 //!< A 64-bit float cell.
};

/** \brief Canonical ABI choices that affect flattening and memory layout. */
struct CanonicalAbiOptions {
	//! The encoding `string.lower`/`string.lift` use.
	enum class StringEncoding : uint8_t {
		Utf8, //!< `utf8`.
		Utf16, //!< `utf16`.
		Latin1Utf16 //!< `latin1+utf16`.
	};
	//! Whether the type is being lifted or lowered (async flattening differs).
	enum class Direction : uint8_t {
		Lift, //!< `canon lift`.
		Lower //!< `canon lower`.
	};
	//! The "no index" marker for an option the function does not name.
	static constexpr uint32_t kNoIndex = ~uint32_t{0};

	//! The string encoding the lowering uses.
	StringEncoding stringEncoding = StringEncoding::Utf8;
	//! The memory index the canonical options name, or `kNoIndex`.
	uint32_t memory = kNoIndex;
	//! The `realloc` function index, or `kNoIndex`.
	uint32_t realloc = kNoIndex;
	//! The `post-return` function index, or `kNoIndex`.
	uint32_t postReturn = kNoIndex;
	//! The async callback function index, or `kNoIndex`.
	uint32_t callback = kNoIndex;
	//! True when the function takes part in the async ABI.
	bool async = false;
	//! Which canonical direction derived this signature.
	Direction direction = Direction::Lift;
	//! Canonical ABI pointer/length width: 4 for memory32, 8 for memory64.
	uint8_t pointerSize = 4;

	//! True when every lowering-relevant option matches.
	bool operator==(const CanonicalAbiOptions& other) const
	{
		return stringEncoding == other.stringEncoding && memory == other.memory
				&& realloc == other.realloc && postReturn == other.postReturn
				&& callback == other.callback && async == other.async
				&& direction == other.direction
				&& pointerSize == other.pointerSize;
	}
	//! True when any lowering-relevant option differs.
	bool operator!=(const CanonicalAbiOptions& other) const
	{
		return !(*this == other);
	}
};

//! A structured host-facing component value. It is intentionally a small
//! discriminated union: the runtime owns conversion to/from the canonical ABI,
//! so callers never see raw pointers into wasm linear memory.
/** \brief Host-facing structured value used by the Component Model runtime. */
struct ComponentValue {
	//! Which representation the value uses.
	enum class Kind : uint8_t {
		Bool, S8, U8, S16, U16, S32, U32, S64, U64, F32, F64, Char,
		String, List, FixedList, Record, Tuple, Variant, Enum, Flags,
		Option, Result, Own, Borrow, Stream, Future, Map
	};
	Kind kind = Kind::Bool; //!< Which representation the value uses.
	//! The scalar payload, reinterpreted according to `kind`.
	union {
		//! A 64-bit signed scalar (the widest numeric payload).
		int64_t  s64 = 0;
		//! A 64-bit unsigned scalar.
		uint64_t u64;
		//! A 64-bit float scalar.
		double   f64;
	};
	//! Primitive value, discriminant/case/flags, or resource handle, depending
	//! on `kind`.
	uint32_t discriminant = 0;
	//! The string payload for a `string` value.
	std::string string;
	//! The element payload for a list, tuple, record or variant value.
	std::vector<ComponentValue> elements;
	std::vector<bool> flags; //!< The bit set of a `flags` value.
	bool hasValue = false; //!< True when `kind` carries a payload.
};

//! A component-level function. `call` is the flat-value entry point retained for
//! scalar-only callers; `callValues` is the structured entry point used when a
//! signature contains strings/lists/records or other memory-backed values.
/** \brief Callable component function with flat and structured invocation forms. */
struct ComponentFunction {
	//! The canonical identity of the component function type.
	WASM::TypeId typeId{WASM::TypeId::kNone};
	//! The runtime that owns this function's resource handle table. Null for a
	//! host-supplied function; a cross-runtime lower moves handles through it.
	class ComponentRuntime* ownerRuntime = nullptr;
	//! The flattened core parameter types.
	std::vector<WASM::AbiCoreType> params;
	std::vector<WASM::AbiCoreType> results; //!< The flattened core result types.
	std::vector<WASM::ComponentValueType> paramTypes; //!< The structured parameter types.
	std::vector<WASM::ComponentValueType> resultTypes; //!< The structured result types.
	WASM::CanonicalAbiOptions options; //!< The Canonical ABI options this function was lowered with.
	bool indirectParams = false; //!< True when arguments travel through a single pointer.
	bool indirectResults = false; //!< True when results are returned through a caller pointer.
	//! The flat-value entry point (scalar-only signatures).
	std::function<void(std::span<const WASM::Value>, std::span<WASM::Value>)> call;
	//! The structured entry point (strings, lists, records, …).
	std::function<void(std::span<const ComponentValue>, std::span<ComponentValue>)> callValues;
};

//! A host-supplied component instance: the export bag an instance import or a
//! non-function `with` argument can carry. A host-supplied bag is self-contained;
//! function entries copied from a live component runtime remain tied to that
//! runtime and must not outlive it.
struct ComponentInstanceValue {
	//! Named function exports.
	std::unordered_map<std::string, ComponentFunction> functions;
	//! Named value exports.
	std::unordered_map<std::string, ComponentValue> values;
	//! Named type exports (their canonical identities).
	std::unordered_map<std::string, WASM::TypeId> types;
	//! Named instance exports.
	std::unordered_map<std::string, std::shared_ptr<ComponentInstanceValue>> instances;
	//! Named component exports.
	std::unordered_map<std::string, std::shared_ptr<const LoadedComponent>> components;
	//! Named core-module exports (host-supplied instance bags and re-exports).
	std::unordered_map<std::string, std::shared_ptr<const Module>> coreModules;

	/** \brief Visits every nested instance export depth-first. \param visitor Callback receiving the export name and bag. */
	void visitNested(const std::function<void(std::string_view,
			const ComponentInstanceValue&)>& visitor) const
	{
		for (const auto& [name, nested] : instances) {
			if (!nested)
				continue;
			visitor(name, *nested);
			nested->visitNested(visitor);
		}
	}
};

//! One component-level import or instantiate argument. A function entry keeps
//! the historical `ComponentFunction` shape through the converting constructor.
struct ComponentImport {
	//! Which sort this import carries.
	enum class Kind : uint8_t {
		Function, //!< A callable function.
		Instance, //!< A host-supplied instance bag.
		Value, //!< A runtime value.
		Type, //!< A compile-time type identity (no runtime object).
		Component, //!< A component definition.
		CoreModule //!< A host-supplied core module definition.
	};
	Kind kind = Kind::Function; //!< Discriminant for the payload below.
	ComponentFunction function; //!< Valid when kind == Function.
	std::shared_ptr<ComponentInstanceValue> instance; //!< Valid when kind == Instance.
	ComponentValue value; //!< Valid when kind == Value.
	WASM::TypeId type{WASM::TypeId::kNone}; //!< Valid when kind == Type.
	std::shared_ptr<const LoadedComponent> component; //!< Valid when kind == Component.
	std::shared_ptr<const Module> coreModule; //!< Valid when kind == CoreModule.

	/** \brief Creates an empty import. */
	ComponentImport() = default;
	/** \brief Creates a function import from the historical descriptor. \param function Callable descriptor. */
	ComponentImport(ComponentFunction function)
		: kind(Kind::Function), function(std::move(function)) {}
	/** \brief Creates an instance import. \param instance Host-supplied export bag. \return The import. */
	static ComponentImport instanceImport(
			std::shared_ptr<ComponentInstanceValue> instance)
	{
		ComponentImport out;
		out.kind = Kind::Instance;
		out.instance = std::move(instance);
		return out;
	}
	/** \brief Creates a value import. \param value Runtime value. \return The import. */
	static ComponentImport valueImport(ComponentValue value)
	{
		ComponentImport out;
		out.kind = Kind::Value;
		out.value = std::move(value);
		return out;
	}
	/** \brief Creates a type import. \param type Canonical type identity. \return The import. */
	static ComponentImport typeImport(WASM::TypeId type)
	{
		ComponentImport out;
		out.kind = Kind::Type;
		out.type = type;
		return out;
	}
	/** \brief Creates a component import. \param component Loaded component definition. \return The import. */
	static ComponentImport componentImport(
			std::shared_ptr<const LoadedComponent> component)
	{
		ComponentImport out;
		out.kind = Kind::Component;
		out.component = std::move(component);
		return out;
	}
	/** \brief Creates a core-module import. \param module Parsed core module definition. \return The import. */
	static ComponentImport coreModuleImport(std::shared_ptr<const Module> module)
	{
		ComponentImport out;
		out.kind = Kind::CoreModule;
		out.coreModule = std::move(module);
		return out;
	}
};

} // namespace WASM

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTVALUE_HPP
