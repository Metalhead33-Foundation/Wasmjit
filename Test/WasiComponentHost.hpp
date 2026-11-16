#ifndef WASICOMPONENTHOST_HPP
#define WASICOMPONENTHOST_HPP
#include "WasiStub.hpp"

#include <WasmBase/WasmComponentValue.hpp>
#include <WasmBase/WasmStore.hpp>
#include <WasmBase/WasmTypeStore.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/*!
  \file WasiComponentHost.hpp
  \brief A fake WASI 0.2 component host: `wasi:io`, `wasi:cli`, `wasi:clocks`,
  `wasi:filesystem` and `wasi:random`, for fixtures only.

  Like `WasiStub.hpp` this is **not** an implementation and must never become one
  (`SCOPE.md` §3 keeps WASI on the embedder's side). It answers the interfaces the
  language arms actually import, with plausible but entirely synthetic behaviour:

  | Interface | What the fake host answers |
  |---|---|
  | `wasi:io/error` | a resource type with no functions |
  | `wasi:io/poll` | every pollable is ready; `poll` returns the indices in order |
  | `wasi:io/streams` | writes are captured in the fixture state, reads are at end of file, `check-write` reports room |
  | `wasi:cli/environment` | an empty environment, one `test` argument |
  | `wasi:cli/exit` | records the status and returns; it never ends the process |
  | `wasi:cli/stdin`, `stdout`, `stderr` | one synthetic stream handle each |
  | `wasi:cli/terminal-*` | `option::none`: this is not a terminal |
  | `wasi:clocks/monotonic-clock` | `WasiStub::state().clockTicks` |
  | `wasi:clocks/wall-clock` | the same ticks as a `datetime` |
  | `wasi:filesystem/types` | one synthetic directory: `stat` is a zeroed record, `get-type` is `directory`, no file is ever opened |
  | `wasi:filesystem/preopens` | one preopen, `/`, whose descriptor is a synthetic handle |
  | `wasi:random/random` | the deterministic pattern `WasiStub::randomByte` |

  No file, socket, terminal or clock is touched, and nothing is printed. The
  resource types are host-owned: the handles are this fixture's own numbers, and
  the drops are registered with the bag so `resource.drop` calls back
  (`ComponentInstanceValue::resourceDrops`).

  Each interface is registered under the version suffixes the study arms use
  (`@0.2.0`, `@0.2.6`, `@0.2.12`) and without one, so a fixture does not have to
  know which toolchain produced it.

  This header is internal: it is not part of the embedding surface.
*/

namespace WasiComponentHost {

//! Builds the component value types the fake host declares, in one store.
class Types {
public:
	//! \brief Binds the builder to the store the component was parsed into.
	//! \param store The runtime's store.
	explicit Types(WASM::Store& store) : store_(store) {}

	//! \brief Wraps a canonical type id as a value type.
	//! \param id The type id.
	//! \return The value type node.
	WASM::ComponentValueType of(WASM::TypeId id) const
	{
		WASM::ComponentValueType value;
		value.kind = WASM::ComponentValueType::Kind::TypeId;
		value.typeIndex = id.value;
		return value;
	}
	//! \brief Interns a primitive value type.
	//! \param primitive The primitive.
	//! \return The value type node.
	WASM::ComponentValueType primitive(WASM::PrimitiveValueType primitive) const
	{
		return of(store_.types().store().addComponentPrimitive(primitive));
	}
	//! \brief The `bool` type.
	//! \return The value type node.
	WASM::ComponentValueType boolean() const
	{
		return primitive(WASM::PrimitiveValueType::Bool);
	}
	//! \brief The `u8` type.
	//! \return The value type node.
	WASM::ComponentValueType u8() const
	{
		return primitive(WASM::PrimitiveValueType::U8);
	}
	//! \brief The `u32` type.
	//! \return The value type node.
	WASM::ComponentValueType u32() const
	{
		return primitive(WASM::PrimitiveValueType::U32);
	}
	//! \brief The `u64` type.
	//! \return The value type node.
	WASM::ComponentValueType u64() const
	{
		return primitive(WASM::PrimitiveValueType::U64);
	}
	//! \brief The `string` type.
	//! \return The value type node.
	WASM::ComponentValueType string() const
	{
		return primitive(WASM::PrimitiveValueType::String);
	}
	//! \brief A `list<T>` type.
	//! \param element The element type.
	//! \return The value type node.
	WASM::ComponentValueType list(const WASM::ComponentValueType& element) const
	{
		return of(store_.types().store().addComponentList(element));
	}
	//! \brief An `option<T>` type.
	//! \param element The payload type.
	//! \return The value type node.
	WASM::ComponentValueType option(const WASM::ComponentValueType& element) const
	{
		return of(store_.types().store().addComponentOption(element));
	}
	//! \brief A tuple type.
	//! \param elements The element types, in order.
	//! \return The value type node.
	WASM::ComponentValueType tuple(
			std::vector<WASM::ComponentValueType> elements) const
	{
		return of(store_.types().store().addComponentTuple(elements));
	}
	//! \brief A record type.
	//! \param fields The fields, in declaration order.
	//! \return The value type node.
	WASM::ComponentValueType record(
			std::vector<std::pair<std::string, WASM::ComponentValueType>> fields)
			const
	{
		WASM::TypeStore& store = store_.types().store();
		std::vector<WASM::ComponentField> out;
		out.reserve(fields.size());
		for (const auto& [name, type] : fields) {
			WASM::ComponentField field;
			field.name = store.internString(name);
			field.type = type;
			out.push_back(field);
		}
		return of(store.addComponentRecord(out));
	}
	//! \brief An enum type.
	//! \param labels The case labels, in order.
	//! \return The value type node.
	WASM::ComponentValueType enumOf(std::vector<std::string> labels) const
	{
		return of(store_.types().store().addComponentLabels(
				WASM::TypeKind::ComponentEnum, intern(labels)));
	}
	//! \brief A flags type.
	//! \param labels The flag labels, in order.
	//! \return The value type node.
	WASM::ComponentValueType flagsOf(std::vector<std::string> labels) const
	{
		return of(store_.types().store().addComponentLabels(
				WASM::TypeKind::ComponentFlags, intern(labels)));
	}
	//! \brief A `result<ok, error>` type; either payload may be absent.
	//! \param ok The ok payload, or nullopt for `_`.
	//! \param error The error payload, or nullopt for `_`.
	//! \return The value type node.
	WASM::ComponentValueType resultOf(
			const std::optional<WASM::ComponentValueType>& ok,
			const std::optional<WASM::ComponentValueType>& error) const
	{
		return of(store_.types().store().addComponentResult(
				ok ? *ok : WASM::ComponentValueType{},
				error ? *error : WASM::ComponentValueType{},
				ok.has_value(), error.has_value()));
	}
	//! \brief A variant type.
	//! \param cases The cases, in declaration order; nullopt means "no payload".
	//! \return The value type node.
	WASM::ComponentValueType variantOf(
			std::vector<std::pair<std::string, std::optional<WASM::ComponentValueType>>>
					cases) const
	{
		WASM::TypeStore& store = store_.types().store();
		std::vector<WASM::ComponentCase> out;
		out.reserve(cases.size());
		for (const auto& [label, type] : cases) {
			WASM::ComponentCase item;
			item.label = store.internString(label);
			item.hasType = type.has_value();
			if (type)
				item.type = *type;
			out.push_back(item);
		}
		return of(store.addComponentVariant(out));
	}
	//! \brief An `own<R>` handle type.
	//! \param resource The resource type.
	//! \return The value type node.
	WASM::ComponentValueType own(WASM::TypeId resource) const
	{
		return of(store_.types().store().addComponentTypeRefCanonical(
				WASM::TypeKind::ComponentOwn, resource));
	}
	//! \brief A `borrow<R>` handle type.
	//! \param resource The resource type.
	//! \return The value type node.
	WASM::ComponentValueType borrow(WASM::TypeId resource) const
	{
		return of(store_.types().store().addComponentTypeRefCanonical(
				WASM::TypeKind::ComponentBorrow, resource));
	}
	//! \brief Mints a nominal abstract resource type.
	//! \return The new resource type id.
	WASM::TypeId resource() const
	{
		WASM::ComponentResourcePayload payload;
		payload.rep.opcode = WASM::ValueTypeCode::I32;
		payload.abstract = true;
		return store_.types().store().addComponentResource(payload);
	}
	//! \brief Interns a label list for enumOf()/flagsOf().
	//! \param labels The labels.
	//! \return The interned ids, in order.
	std::vector<WASM::StringId> intern(const std::vector<std::string>& labels) const
	{
		std::vector<WASM::StringId> out;
		out.reserve(labels.size());
		for (const std::string& label : labels)
			out.push_back(store_.types().store().internString(label));
		return out;
	}

private:
	//! The store the types are interned in.
	WASM::Store& store_;
};

//! The fake host's handle space: one counter, and the type each live handle has.
class Handles {
public:
	//! \brief Mints a handle for a resource type.
	//! \param type The resource type the handle belongs to.
	//! \return The new handle, never zero.
	std::uint32_t create(WASM::TypeId type)
	{
		const std::uint32_t handle = next_++;
		live_.emplace(handle, type.value);
		return handle;
	}
	//! \brief Reports whether a handle is live and has the expected type.
	//! \param handle The handle the guest passed.
	//! \param type The expected resource type.
	//! \return true when the handle belongs to that type and has not been dropped.
	bool contains(std::uint32_t handle, WASM::TypeId type) const
	{
		const auto found = live_.find(handle);
		return found != live_.end() && found->second == type.value;
	}
	//! \brief Releases a handle, whatever its type.
	//! \param handle The handle to forget.
	void drop(std::uint32_t handle)
	{
		live_.erase(handle);
	}
	//! \brief How many handles are live.
	//! \return The live handle count.
	std::size_t size() const { return live_.size(); }

private:
	std::uint32_t next_ = 1; //!< The next handle to mint.
	std::unordered_map<std::uint32_t, std::uint32_t> live_; //!< handle -> type id.
};

//! \brief Builds an `own<R>` value from a handle.
//! \param handle The host handle.
//! \return The component value.
inline WASM::ComponentValue ownValue(std::uint32_t handle)
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::Own;
	value.discriminant = handle;
	return value;
}
//! \brief Builds a `u64` value.
//! \param number The number.
//! \return The component value.
inline WASM::ComponentValue u64Value(std::uint64_t number)
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::U64;
	value.u64 = number;
	return value;
}
//! \brief Builds a `u32` value.
//! \param number The number.
//! \return The component value.
inline WASM::ComponentValue u32Value(std::uint32_t number)
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::U32;
	value.u64 = number;
	return value;
}
//! \brief Builds a `string` value.
//! \param text The bytes.
//! \return The component value.
inline WASM::ComponentValue stringValue(std::string text)
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::String;
	value.string = std::move(text);
	return value;
}
//! \brief Builds a `list<T>` value.
//! \param elements The elements, in order.
//! \return The component value.
inline WASM::ComponentValue listValue(
		std::vector<WASM::ComponentValue> elements = {})
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::List;
	value.elements = std::move(elements);
	return value;
}
//! \brief Builds a tuple value.
//! \param elements The elements, in order.
//! \return The component value.
inline WASM::ComponentValue tupleValue(std::vector<WASM::ComponentValue> elements)
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::Tuple;
	value.elements = std::move(elements);
	return value;
}
//! \brief Builds a record value.
//! \param fields The field values, in declaration order.
//! \return The component value.
inline WASM::ComponentValue recordValue(
		std::vector<WASM::ComponentValue> fields)
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::Record;
	value.elements = std::move(fields);
	return value;
}

//! \brief Builds a `result::ok` value with no payload.
//! \return The component value.
inline WASM::ComponentValue okResult()
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::Result;
	value.discriminant = 0;
	return value;
}
//! \brief Builds a `result::ok` value with a payload.
//! \param payload The ok payload.
//! \return The component value.
inline WASM::ComponentValue okResult(WASM::ComponentValue payload)
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::Result;
	value.discriminant = 0;
	value.hasValue = true;
	value.elements.push_back(std::move(payload));
	return value;
}
//! \brief Builds a `result::error` value with no payload.
//! \return The component value.
inline WASM::ComponentValue errorResult()
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::Result;
	value.discriminant = 1;
	return value;
}
//! \brief Builds a `result::error` value with a payload.
//! \param payload The error payload.
//! \return The component value.
inline WASM::ComponentValue errorResult(WASM::ComponentValue payload)
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::Result;
	value.discriminant = 1;
	value.hasValue = true;
	value.elements.push_back(std::move(payload));
	return value;
}
//! \brief Builds an `option::none` value.
//! \return The component value.
inline WASM::ComponentValue noneOption()
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::Option;
	value.discriminant = 0;
	return value;
}
//! \brief Builds an `option::some` value.
//! \param payload The payload.
//! \return The component value.
inline WASM::ComponentValue someOption(WASM::ComponentValue payload)
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::Option;
	value.discriminant = 1;
	value.hasValue = true;
	value.elements.push_back(std::move(payload));
	return value;
}
//! \brief Builds an enum value.
//! \param caseIndex The case index.
//! \return The component value.
inline WASM::ComponentValue enumValue(std::uint32_t caseIndex)
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::Enum;
	value.discriminant = caseIndex;
	return value;
}
//! \brief Builds a flags value.
//! \param bits The set bits.
//! \param count How many flags the type declares.
//! \return The component value.
inline WASM::ComponentValue flagsValue(std::uint32_t bits, std::size_t count)
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::Flags;
	value.discriminant = bits;
	value.flags.assign(count, false);
	for (std::size_t i = 0; i < count && i < 32; ++i)
		value.flags[i] = (bits & (std::uint32_t{1} << i)) != 0;
	return value;
}
//! \brief Builds the `stream-error::closed` variant value.
//! \return The component value.
inline WASM::ComponentValue closedStreamError()
{
	WASM::ComponentValue value;
	value.kind = WASM::ComponentValue::Kind::Variant;
	value.discriminant = 1;
	return value;
}

//! The fake host: the resource types it owns and the bags it publishes.
class Host : public std::enable_shared_from_this<Host> {
public:
	//! \brief Creates a host bound to one store.
	//! \param store The store the component was parsed into.
	//! \return The host, shared so its callbacks outlive the fixture value.
	static std::shared_ptr<Host> create(WASM::Store& store)
	{
		return std::shared_ptr<Host>(new Host(store));
	}
	//! \brief Builds every interface bag, under every version suffix.
	//! \return The import map to pass to `Runtime::instantiateComponent`.
	WASM::ComponentHostImports imports();
	//! \brief The handles the fake host still owns.
	//! \return The handle registry.
	Handles& handles() { return *handles_; }
	//! \brief The `wasi:io/error` resource type this host exports.
	//! \return The resource type id.
	WASM::TypeId errorType() const { return error_; }
	//! \brief The `wasi:filesystem/types` descriptor resource type.
	//! \return The resource type id.
	WASM::TypeId descriptorType() const { return descriptor_; }

private:
	//! \brief Binds the host to a store and mints its resource types.
	//! \param store The store the component was parsed into.
	explicit Host(WASM::Store& store);
	//! \brief Traces one host call when `WASMJIT_STUB_TRACE` is set.
	//! \param name The interface function name.
	static void trace(const char* name)
	{
		static const bool on = std::getenv("WASMJIT_STUB_TRACE") != nullptr;
		if (on)
			std::fprintf(stderr, "wasi stub call: %s\n", name);
	}

	//! \brief Builds a host function whose flat signature comes from the store.
	//! \param params The structured parameter types.
	//! \param results The structured result types.
	//! \param body The host body.
	//! \return The component function to put in a bag.
	WASM::ComponentFunction function(
			std::vector<WASM::ComponentValueType> params,
			std::vector<WASM::ComponentValueType> results,
			std::function<void(std::span<const WASM::ComponentValue>,
					std::span<WASM::ComponentValue>)> body) const;
	//! \brief Builds the `wasi:io/error` bag.
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> ioError();
	//! \brief Builds the `wasi:io/poll` bag.
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> ioPoll();
	//! \brief Builds the `wasi:io/streams` bag.
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> ioStreams();
	//! \brief Builds the `wasi:cli/environment` bag.
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> cliEnvironment();
	//! \brief Builds the `wasi:cli/exit` bag.
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> cliExit();
	//! \brief Builds one stdio bag.
	//! \param name `get-stdin`, `get-stdout` or `get-stderr`.
	//! \param input true for an input stream, false for an output stream.
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> cliStdio(const char* name,
			bool input);
	//! \brief Builds the `wasi:cli/terminal-input` bag (types only).
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> cliTerminalInput();
	//! \brief Builds the `wasi:cli/terminal-output` bag (types only).
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> cliTerminalOutput();
	//! \brief Builds one terminal getter bag.
	//! \param name The getter name.
	//! \param output true for a terminal-output getter.
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> cliTerminalGetter(
			const char* name, bool output);
	//! \brief Builds the `wasi:clocks/monotonic-clock` bag.
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> monotonicClock();
	//! \brief Builds the `wasi:clocks/wall-clock` bag.
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> wallClock();
	//! \brief Builds the `wasi:filesystem/types` bag.
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> filesystemTypes();
	//! \brief Builds the `wasi:filesystem/preopens` bag.
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> filesystemPreopens();
	//! \brief Builds the `wasi:random/random` bag.
	//! \return The bag.
	std::shared_ptr<WASM::ComponentInstanceValue> random();

	WASM::Store& store_; //!< The store the types are interned in.
	Types types_; //!< The value-type builder.
	std::shared_ptr<Handles> handles_; //!< The host's own handle space.
	WASM::TypeId error_; //!< `wasi:io/error.error`.
	WASM::TypeId pollable_; //!< `wasi:io/poll.pollable`.
	WASM::TypeId outputStream_; //!< `wasi:io/streams.output-stream`.
	WASM::TypeId inputStream_; //!< `wasi:io/streams.input-stream`.
	WASM::TypeId terminalInput_; //!< `wasi:cli/terminal-input.terminal-input`.
	WASM::TypeId terminalOutput_; //!< `wasi:cli/terminal-output.terminal-output`.
	WASM::TypeId descriptor_; //!< `wasi:filesystem/types.descriptor`.
	WASM::TypeId directoryEntryStream_; //!< `wasi:filesystem/types.directory-entry-stream`.
};

inline Host::Host(WASM::Store& store)
	: store_(store), types_(store), handles_(std::make_shared<Handles>())
{
	error_ = types_.resource();
	pollable_ = types_.resource();
	outputStream_ = types_.resource();
	inputStream_ = types_.resource();
	terminalInput_ = types_.resource();
	terminalOutput_ = types_.resource();
	descriptor_ = types_.resource();
	directoryEntryStream_ = types_.resource();
}

inline WASM::ComponentFunction Host::function(
		std::vector<WASM::ComponentValueType> params,
		std::vector<WASM::ComponentValueType> results,
		std::function<void(std::span<const WASM::ComponentValue>,
				std::span<WASM::ComponentValue>)> body) const
{
	WASM::ComponentFunction out;
	out.paramTypes = std::move(params);
	out.resultTypes = std::move(results);
	const WASM::TypeStore& store = store_.types().store();
	for (const WASM::ComponentValueType& param : out.paramTypes)
		if (!store.canonicalAbiFlatValue(param, out.params, 4))
			throw std::runtime_error(
					"wasi component stub: a parameter type is not flattenable");
	for (const WASM::ComponentValueType& result : out.resultTypes)
		if (!store.canonicalAbiFlatValue(result, out.results, 4))
			throw std::runtime_error(
					"wasi component stub: a result type is not flattenable");
	out.callValues = std::move(body);
	return out;
}

inline WASM::ComponentHostImports Host::imports()
{
	std::unordered_map<std::string,
			std::shared_ptr<WASM::ComponentInstanceValue>> bags;
	bags.emplace("wasi:io/error", ioError());
	bags.emplace("wasi:io/poll", ioPoll());
	bags.emplace("wasi:io/streams", ioStreams());
	bags.emplace("wasi:cli/environment", cliEnvironment());
	bags.emplace("wasi:cli/exit", cliExit());
	bags.emplace("wasi:cli/stdin", cliStdio("get-stdin", true));
	bags.emplace("wasi:cli/stdout", cliStdio("get-stdout", false));
	bags.emplace("wasi:cli/stderr", cliStdio("get-stderr", false));
	bags.emplace("wasi:cli/terminal-input", cliTerminalInput());
	bags.emplace("wasi:cli/terminal-output", cliTerminalOutput());
	bags.emplace("wasi:cli/terminal-stdin",
			cliTerminalGetter("get-terminal-stdin", false));
	bags.emplace("wasi:cli/terminal-stdout",
			cliTerminalGetter("get-terminal-stdout", true));
	bags.emplace("wasi:cli/terminal-stderr",
			cliTerminalGetter("get-terminal-stderr", true));
	bags.emplace("wasi:clocks/monotonic-clock", monotonicClock());
	bags.emplace("wasi:clocks/wall-clock", wallClock());
	bags.emplace("wasi:filesystem/types", filesystemTypes());
	bags.emplace("wasi:filesystem/preopens", filesystemPreopens());
	bags.emplace("wasi:random/random", random());
	//! A fixture should not have to know which toolchain produced the artifact,
	//! so every interface is published under each version suffix the arms use.
	static const char* const kVersions[] = { "@0.2.0", "@0.2.6", "@0.2.12", "" };
	//! One trace point per host function, installed here so a fixture (or the
	//! study runner) can name the call that trapped with `WASMJIT_STUB_TRACE`.
	for (auto& [interface, bag] : bags)
		for (auto& [name, function] : bag->functions) {
			auto body = function.callValues;
			const std::string label = interface + "#" + name;
			function.callValues =
					[body = std::move(body), label](std::span<const WASM::ComponentValue> args,
							std::span<WASM::ComponentValue> results) {
						trace(label.c_str());
						body(args, results);
					};
		}
	WASM::ComponentHostImports out;
	for (const auto& [name, bag] : bags)
		for (const char* version : kVersions)
			out.emplace(name + version,
					WASM::ComponentImport::instanceImport(bag));
	return out;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::ioError()
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->types.emplace("error", error_);
	bag->resourceDrops.emplace(error_.value,
			[handles = handles_](std::uint32_t handle) { handles->drop(handle); });
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::ioPoll()
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->types.emplace("pollable", pollable_);
	bag->functions.emplace("poll",
			function({ types_.list(types_.borrow(pollable_)) },
					{ types_.list(types_.u32()) },
					[](std::span<const WASM::ComponentValue> args,
							std::span<WASM::ComponentValue> results) {
						//! Everything the fake host hands out is ready at once.
						std::vector<WASM::ComponentValue> ready;
						if (!args.empty())
							for (std::size_t i = 0; i < args[0].elements.size(); ++i)
								ready.push_back(u32Value(
										static_cast<std::uint32_t>(i)));
						results[0] = listValue(std::move(ready));
					}));
	bag->resourceDrops.emplace(pollable_.value,
			[handles = handles_](std::uint32_t handle) { handles->drop(handle); });
	//! wasi-sdk 34's pollable also carries `block`; the fake host is never blocked.
	bag->functions.emplace("[method]pollable.block",
			function({ types_.borrow(pollable_) }, {},
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue>) {}));
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::ioStreams()
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->types.emplace("output-stream", outputStream_);
	bag->types.emplace("input-stream", inputStream_);
	const WASM::ComponentValueType streamError = types_.variantOf({
			{ "last-operation-failed", types_.own(error_) },
			{ "closed", std::nullopt } });
	const WASM::ComponentValueType writeResult =
			types_.resultOf(std::nullopt, streamError);
	const WASM::ComponentValueType borrowOutput = types_.borrow(outputStream_);
	const WASM::ComponentValueType borrowInput = types_.borrow(inputStream_);
	//! Every write lands in the fixture state; nothing reaches a console.
	const auto capture = [](std::span<const WASM::ComponentValue> args) {
		if (args.size() < 2)
			return;
		for (const WASM::ComponentValue& byte : args[1].elements)
			WasiStub::state().streamBytes.push_back(
					static_cast<char>(byte.u64));
		++WasiStub::state().streamWrites;
	};
	const auto subscribe = [handles = handles_, pollable = pollable_](
								   std::span<const WASM::ComponentValue>,
								   std::span<WASM::ComponentValue> results) {
		results[0] = ownValue(handles->create(pollable));
	};
	bag->functions.emplace("[method]output-stream.check-write",
			function({ borrowOutput },
					{ types_.resultOf(types_.u64(), streamError) },
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						results[0] = okResult(u64Value(4096));
					}));
	bag->functions.emplace("[method]output-stream.write",
			function({ borrowOutput, types_.list(types_.u8()) }, { writeResult },
					[capture](std::span<const WASM::ComponentValue> args,
							std::span<WASM::ComponentValue> results) {
						capture(args);
						results[0] = okResult();
					}));
	bag->functions.emplace("[method]output-stream.blocking-write-and-flush",
			function({ borrowOutput, types_.list(types_.u8()) }, { writeResult },
					[capture](std::span<const WASM::ComponentValue> args,
							std::span<WASM::ComponentValue> results) {
						capture(args);
						results[0] = okResult();
					}));
	bag->functions.emplace("[method]output-stream.blocking-flush",
			function({ borrowOutput }, { writeResult },
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						results[0] = okResult();
					}));
	bag->functions.emplace("[method]output-stream.subscribe",
			function({ borrowOutput }, { types_.own(pollable_) }, subscribe));
	bag->functions.emplace("[method]input-stream.subscribe",
			function({ borrowInput }, { types_.own(pollable_) }, subscribe));
	for (const WASM::TypeId type : { outputStream_, inputStream_ })
		bag->resourceDrops.emplace(type.value,
				[handles = handles_](std::uint32_t handle) {
					handles->drop(handle);
				});
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::cliEnvironment()
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->functions.emplace("get-environment",
			function({},
					{ types_.list(types_.tuple(
							{ types_.string(), types_.string() })) },
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						//! One variable rather than none: a guest that allocates
						//! the returned list is exercising its own `cabi_realloc`,
						//! and a zero-length list would ask it for nothing.
						results[0] = listValue({ tupleValue(
								{ stringValue("WASMJIT_STUB"),
										stringValue("1") }) });
					}));
	bag->functions.emplace("get-arguments",
			function({}, { types_.list(types_.string()) },
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						results[0] = listValue({ stringValue("test") });
					}));
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::cliExit()
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->functions.emplace("exit",
			function({ types_.resultOf(std::nullopt, std::nullopt) }, {},
					[](std::span<const WASM::ComponentValue> args,
							std::span<WASM::ComponentValue>) {
						//! A fake host cannot end the process, and should not try:
						//! the status is recorded and the call returns.
						WasiStub::state().exitCalled = true;
						WasiStub::state().exitCode =
								(args.empty() || args[0].discriminant == 0) ? 0 : 1;
					}));
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::cliStdio(
		const char* name, bool input)
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	const WASM::TypeId type = input ? inputStream_ : outputStream_;
	bag->functions.emplace(name,
			function({}, { types_.own(type) },
					[handles = handles_, type](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						results[0] = ownValue(handles->create(type));
					}));
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::cliTerminalInput()
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->types.emplace("terminal-input", terminalInput_);
	bag->resourceDrops.emplace(terminalInput_.value,
			[handles = handles_](std::uint32_t handle) { handles->drop(handle); });
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::cliTerminalOutput()
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->types.emplace("terminal-output", terminalOutput_);
	bag->resourceDrops.emplace(terminalOutput_.value,
			[handles = handles_](std::uint32_t handle) { handles->drop(handle); });
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::cliTerminalGetter(
		const char* name, bool output)
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	const WASM::TypeId type = output ? terminalOutput_ : terminalInput_;
	bag->functions.emplace(name,
			function({}, { types_.option(types_.own(type)) },
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						//! No fixture runs on a terminal.
						results[0] = noneOption();
					}));
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::monotonicClock()
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	const auto subscribe = [handles = handles_, pollable = pollable_](
								   std::span<const WASM::ComponentValue>,
								   std::span<WASM::ComponentValue> results) {
		results[0] = ownValue(handles->create(pollable));
	};
	bag->functions.emplace("now",
			function({}, { types_.u64() },
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						++WasiStub::state().clockCalls;
						results[0] = u64Value(WasiStub::state().clockTicks);
					}));
	bag->functions.emplace("subscribe-instant",
			function({ types_.u64() }, { types_.own(pollable_) }, subscribe));
	bag->functions.emplace("subscribe-duration",
			function({ types_.u64() }, { types_.own(pollable_) }, subscribe));
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::wallClock()
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->functions.emplace("now",
			function({},
					{ types_.record({ { "seconds", types_.u64() },
							{ "nanoseconds", types_.u32() } }) },
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						const std::uint64_t ticks = WasiStub::state().clockTicks;
						results[0] = recordValue({ u64Value(ticks / 1000),
								u32Value(static_cast<std::uint32_t>(
										(ticks % 1000) * 1000)) });
					}));
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::random()
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->functions.emplace("get-random-bytes",
			function({ types_.u64() }, { types_.list(types_.u8()) },
					[](std::span<const WASM::ComponentValue> args,
							std::span<WASM::ComponentValue> results) {
						++WasiStub::state().randomCalls;
						const std::uint64_t requested =
								args.empty() ? 0 : args[0].u64;
						//! A stub must not allocate whatever the guest asks for.
						const std::size_t count = static_cast<std::size_t>(
								std::min<std::uint64_t>(requested, 4096));
						std::vector<WASM::ComponentValue> bytes;
						bytes.reserve(count);
						for (std::size_t i = 0; i < count; ++i) {
							WASM::ComponentValue byte;
							byte.kind = WASM::ComponentValue::Kind::U8;
							byte.u64 = WasiStub::randomByte(
									static_cast<std::uint32_t>(i));
							bytes.push_back(byte);
						}
						results[0] = listValue(std::move(bytes));
					}));
	bag->functions.emplace("get-random-u64",
			function({}, { types_.u64() },
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						++WasiStub::state().randomCalls;
						results[0] = u64Value(0x5eed5eed5eed5eedull);
					}));
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::filesystemTypes()
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->types.emplace("descriptor", descriptor_);
	bag->types.emplace("directory-entry-stream", directoryEntryStream_);
	const WASM::ComponentValueType errorCode = types_.enumOf({ "access",
			"would-block", "already", "bad-descriptor", "busy", "deadlock",
			"quota", "exist", "file-too-large", "illegal-byte-sequence",
			"in-progress", "interrupted", "invalid", "io", "is-directory", "loop",
			"too-many-links", "message-size", "name-too-long", "no-device",
			"no-entry", "no-lock", "insufficient-memory", "insufficient-space",
			"not-directory", "not-empty", "not-recoverable", "unsupported",
			"no-tty", "no-such-device", "overflow", "not-permitted", "pipe",
			"read-only", "invalid-seek", "text-file-busy", "cross-device" });
	const WASM::ComponentValueType descriptorFlags = types_.flagsOf(
			{ "read", "write", "file-integrity-sync", "data-integrity-sync",
					"requested-write-sync", "mutate-directory" });
	const WASM::ComponentValueType descriptorType = types_.enumOf({ "unknown",
			"block-device", "character-device", "directory", "fifo",
			"symbolic-link", "regular-file", "socket" });
	const WASM::ComponentValueType dateTime =
			types_.record({ { "seconds", types_.u64() },
					{ "nanoseconds", types_.u32() } });
	const WASM::ComponentValueType descriptorStat = types_.record(
			{ { "type", descriptorType },
					{ "link-count", types_.u64() }, { "size", types_.u64() },
					{ "data-access-timestamp", types_.option(dateTime) },
					{ "data-modification-timestamp", types_.option(dateTime) },
					{ "status-change-timestamp", types_.option(dateTime) } });
	const WASM::ComponentValueType borrowDescriptor =
			types_.borrow(descriptor_);
	//! The fake filesystem has one empty directory and no files at all.
	const auto stat = [descriptorStat](std::span<const WASM::ComponentValue>,
							 std::span<WASM::ComponentValue> results) {
		results[0] = okResult(recordValue({ enumValue(3), u64Value(1),
				u64Value(0), noneOption(), noneOption(), noneOption() }));
		(void)descriptorStat;
	};
	bag->functions.emplace("[method]descriptor.read-via-stream",
			function({ borrowDescriptor, types_.u64() },
					{ types_.resultOf(types_.own(inputStream_), errorCode) },
					[handles = handles_, type = inputStream_](
							std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						results[0] = okResult(ownValue(handles->create(type)));
					}));
	bag->functions.emplace("[method]descriptor.write-via-stream",
			function({ borrowDescriptor, types_.u64() },
					{ types_.resultOf(types_.own(outputStream_), errorCode) },
					[handles = handles_, type = outputStream_](
							std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						results[0] = okResult(ownValue(handles->create(type)));
					}));
	bag->functions.emplace("[method]descriptor.append-via-stream",
			function({ borrowDescriptor },
					{ types_.resultOf(types_.own(outputStream_), errorCode) },
					[handles = handles_, type = outputStream_](
							std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						results[0] = okResult(ownValue(handles->create(type)));
					}));
	bag->functions.emplace("[method]descriptor.get-flags",
			function({ borrowDescriptor },
					{ types_.resultOf(descriptorFlags, errorCode) },
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						results[0] = okResult(flagsValue(0x3, 6));
					}));
	bag->functions.emplace("[method]descriptor.get-type",
			function({ borrowDescriptor },
					{ types_.resultOf(descriptorType, errorCode) },
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						results[0] = okResult(enumValue(3));
					}));
	bag->functions.emplace("[method]descriptor.stat",
			function({ borrowDescriptor },
					{ types_.resultOf(descriptorStat, errorCode) }, stat));
	//! .NET's runtime asks for this even when it only stats the preopen.
	bag->functions.emplace("[method]descriptor.metadata-hash",
			function({ borrowDescriptor },
					{ types_.resultOf(
							types_.record({ { "lower", types_.u64() },
									{ "upper", types_.u64() } }),
							errorCode) },
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						results[0] = okResult(recordValue(
								{ u64Value(0), u64Value(0) }));
					}));
	bag->functions.emplace("filesystem-error-code",
			function({ types_.borrow(error_) }, { types_.option(errorCode) },
					[](std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						results[0] = noneOption();
					}));
	for (const WASM::TypeId type : { descriptor_, directoryEntryStream_ })
		bag->resourceDrops.emplace(type.value,
				[handles = handles_](std::uint32_t handle) {
					handles->drop(handle);
				});
	return bag;
}

inline std::shared_ptr<WASM::ComponentInstanceValue> Host::filesystemPreopens()
{
	auto bag = std::make_shared<WASM::ComponentInstanceValue>();
	bag->functions.emplace("get-directories",
			function({},
					{ types_.list(types_.tuple(
							{ types_.own(descriptor_), types_.string() })) },
					[handles = handles_, type = descriptor_](
							std::span<const WASM::ComponentValue>,
							std::span<WASM::ComponentValue> results) {
						results[0] = listValue({ tupleValue(
								{ ownValue(handles->create(type)),
										stringValue("/") }) });
					}));
	return bag;
}

} // namespace WasiComponentHost

#endif // WASICOMPONENTHOST_HPP
