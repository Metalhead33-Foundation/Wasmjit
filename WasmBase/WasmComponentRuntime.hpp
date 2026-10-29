#ifndef WASMCOMPONENTRUNTIME_HPP
#define WASMCOMPONENTRUNTIME_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentRuntime.hpp
  \brief The backend-neutral component execution runtime.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

//! Opens the exported-visibility block; every declaration below is exported
//! (docs/EMBEDDING_SURFACE.md ES-4).
WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"
#include "WasmComponent.hpp"
#include "WasmComponentAbi.hpp"
#include "WasmComponentAsync.hpp"
#include "WasmImport.hpp"
#include "WasmModuleInstance.hpp"
#include "WasmTypeStore.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace WASM {

//! A structured host-facing component value. It is intentionally a small
//! discriminated union: the runtime owns conversion to/from the canonical ABI,
//! so callers never see raw pointers into wasm linear memory.
struct ComponentValue {
	//! Which of the Component Model value shapes this value is: a primitive
	//! scalar, a memory-backed aggregate, or a handle.
	enum class Kind : uint8_t {
		//! Primitive scalars: `bool`, the signed/unsigned 8/16/32/64-bit
		//! integers, the two floats, and `Char` (one Unicode scalar value).
		Bool, S8, U8, S16, U16, S32, U32, S64, U64, F32, F64, Char,
		//! Memory-backed and aggregate shapes: a string, a list, a fixed-length
		//! list, a record, a tuple, a variant, an enum or a flags set.
		String, List, FixedList, Record, Tuple, Variant, Enum, Flags,
		//! Optional, result, resource-handle and async shapes: `Option`,
		//! `Result`, owned/borrowed resources, streams, futures and maps.
		Option, Result, Own, Borrow, Stream, Future, Map
	};
	Kind kind = Kind::Bool; //!< Which payload member below holds the value
	//! The primitive payload; `kind` decides which member is meaningful.
	union {
		int64_t  s64 = 0; //!< Signed integers and `Char`
		uint64_t u64; //!< Unsigned integers, enums and flag words
		double   f64; //!< `F32`/`F64` payload, stored widened
	};
	//! Primitive value, discriminant/case/flags, or resource handle, depending
	//! on `kind`.
	uint32_t discriminant = 0;
	std::string string; //!< `String` payload, decoded to UTF-8
	std::vector<ComponentValue> elements; //!< List/tuple/record/case payloads
	std::vector<bool> flags; //!< `Flags` payload, one bit per declared flag
	bool hasValue = false; //!< True once a payload was decoded into this value
};

//! A component-level function. `call` is the flat-value entry point retained for
//! scalar-only callers; `callValues` is the structured entry point used when a
//! signature contains strings/lists/records or other memory-backed values.
struct ComponentFunction {
	WASM::TypeId typeId{WASM::TypeId::kNone}; //!< Store id of the func type
	std::vector<WASM::AbiCoreType> params; //!< Flattened core parameter types
	std::vector<WASM::AbiCoreType> results; //!< Flattened core result types
	//! Declared component value types of the parameters, in argument order.
	std::vector<WASM::ComponentValueType> paramTypes;
	//! Declared component value types of the results, in result order.
	std::vector<WASM::ComponentValueType> resultTypes;
	WASM::CanonicalAbiOptions options; //!< Canonical ABI options it was built for
	//! True when the parameters are passed indirectly: a pointer to a record of
	//! lifted values instead of one core value per parameter.
	bool indirectParams = false;
	//! True when the results are returned indirectly, through a linear-memory
	//! pointer rather than as one core value per result.
	bool indirectResults = false;
	//! Flat-value entry point: takes the already-lowered core values and writes
	//! the core results; empty when this function was never bound.
	std::function<void(std::span<const WASM::Value>, std::span<WASM::Value>)> call;
	//! Structured entry point: lifts/lowers between `ComponentValue` and the
	//! canonical ABI, so memory-backed values cross the boundary by value.
	std::function<void(std::span<const ComponentValue>, std::span<ComponentValue>)> callValues;
};

//! Executable component runtime for embedded core modules/instances, nested
//! components and `canon lift`/`lower`. It owns the component graph, the
//! canonical ABI value lowering/lifting, resource/stream/future handles and the
//! single-agent async surface. Core-module instantiation is delegated to a
//! `ModuleInstantiator`, so this class is independent of the code generator.
class WASMJIT_API ComponentRuntime {
public:
	//! Host-supplied functions, keyed by the import name they satisfy.
	using ComponentImports = std::unordered_map<std::string, ComponentFunction>;

	//! Builds the runtime for one component and instantiates its graph.
	//! \param [in] instantiator Backend-side core-module instantiator; it is
	//! called for every embedded core module and must outlive this runtime.
	//! \param [in] component The validated component to execute; the runtime
	//! keeps a reference and never copies the AST.
	//! \param [in] imports Host functions that satisfy top-level component
	//! imports, by import name; empty means every import must come from the
	//! component itself.
	//! \throws std::runtime_error when the definition walk fails: an
	//! out-of-bounds index, an unsupported instantiation form, a canonical
	//! definition it cannot lower, or a component value with trailing bytes.
	ComponentRuntime(ModuleInstantiator& instantiator,
			const Component& component, ComponentImports imports = {});
	//! Releases the canonical-ABI closures, call interfaces and trampoline
	//! payloads this runtime allocated; core instances are released with the
	//! owning vectors. The component and the instantiator are not owned.
	~ComponentRuntime();

	//! Copy construction is deleted: a runtime owns core instances, handle
	//! tables and native closures, so it is neither copyable nor movable.
	//! \param [in] ComponentRuntime The source that is never copied (the
	//! parameter is unnamed because no definition can exist).
	//! \return Never returns: the overload is deleted, so any use is a
	//! compile-time error.
	ComponentRuntime(const ComponentRuntime&) = delete;
	//! Copy assignment is deleted for the same reason as copy construction.
	ComponentRuntime& operator=(const ComponentRuntime&) = delete;

	//! The store this component's types and entities live in. Referenced by the
	//! canonical-ABI callbacks, which have no member context of their own.
	//! \return The Store that owns this component's types and memories; it
	//! outlives the runtime because the component keeps a reference to it.
	Store& store() const { return component_.store(); }

	//! Looks up a top-level component function by export name.
	//! \param [in] name The export name, matched byte-exactly against the names
	//! recorded by the definition walk.
	//! \return The function when this component exports it, or nullopt when the
	//! name is not an exported component function.
	std::optional<ComponentFunction> exportedFunction(std::string_view name) const;
	//! The instantiated core function at a core function index.
	//! \param [in] index Index into the runtime's core function space, which
	//! follows the component's core instance order.
	//! \return The callable; it is null-initialised for a slot the graph never
	//! filled. The call throws std::out_of_range when `index` is past the end
	//! of that space.
	WASM::Callable coreFunction(std::size_t index) const;
	//! The instantiated core memory at a core memory index.
	//! \param [in] index Index into the runtime's core memory space.
	//! \return The linear memory; the call throws std::out_of_range when
	//! `index` is past the end of that space.
	WASM::LinearMemory* coreMemory(std::size_t index) const;
	//! Instance-owned Canonical ABI signatures (TYPE_MODEL TM-15). Store metadata
	//! is process-wide; this table is per component instance.
	//! \return The ABI view of this instance, which owns the signatures its
	//! lifts and lowers use and is not shared with any other instance.
	const WASM::ComponentAbiInstance& abi() const { return abi_; }

	//! Resource-handle validation used by the canonical value walkers.
	//! \param [in] handle Handle index in the runtime's handle space; 0 and
	//! indices past the table are rejected.
	//! \param [in] expected Resource type the handle must have; a `kNone` id
	//! accepts any type.
	//! \param [in] borrow True to hand out a borrowed view of an owned handle,
	//! false to pass the handle itself through unchanged.
	//! \param [in] scope Borrow scope id the borrowed slot belongs to, so
	//! endBorrowScope() can release it.
	//! \return The handle the caller should use: `handle` itself when `borrow`
	//! is false, otherwise a freshly reserved borrowed slot.
	//! \throws std::runtime_error when the handle is invalid or of the wrong
	//! type, or when a borrowed handle is lent again.
	uint32_t useResourceHandle(uint32_t handle, WASM::TypeId expected,
			bool borrow, uint32_t scope);
	//! Opens a borrow scope: every borrowed handle handed out from now on is
	//! tagged with the new scope.
	//! \return The new scope id, unique for this runtime and never 0.
	uint32_t beginBorrowScope();
	//! Closes a borrow scope and releases every borrowed slot it handed out.
	//! \param [in] scope Scope id returned by beginBorrowScope(); slots tagged
	//! with another scope are left alone.
	void endBorrowScope(uint32_t scope);
	//! Validates one resource handle without handing anything out.
	//! \param [in] handle Handle index in the runtime's handle space.
	//! \param [in] expected Resource type the handle must have; a `kNone` id
	//! accepts any type.
	//! \throws std::runtime_error when the handle is 0, past the table, no
	//! longer live, or of a different resource type.
	void validateResourceHandle(uint32_t handle, WASM::TypeId expected) const;

	//! Private helpers: the per-call ABI view and this instance's own state.
private:
	//! The core-side view of one canonical call: where lifted strings and lists
	//! live, and the functions that allocate and release that memory.
	struct AbiMemory {
		WASM::LinearMemory* memory = nullptr; //!< Canonical memory, if named
		WASM::Callable realloc; //!< Canonical realloc, if named
		WASM::Callable postReturn; //!< Canonical post-return, if named
		uint8_t pointerSize = 4; //!< 4 or 8: the canonical pointer width
	};

	//! One slot of the handle table shared by resources, streams and futures.
	struct ResourceSlot {
		uint64_t representation = 0; //!< `resource.rep` value of the resource
		WASM::TypeId typeId{WASM::TypeId::kNone}; //!< Resource type of the slot
		bool live = false; //!< True while the slot is in use
		bool own = true; //!< True when this instance owns (may drop) the resource
		uint32_t borrowScope = 0; //!< Scope that lent it; 0 when owned
	};

	//! A core instance in the component graph: either an instantiated core
	//! module or an inline bag of core exports (the form canon built-ins use to
	//! reach a core module's imports).
	struct CoreInstanceHandle {
		//! The instantiated core module; null when this is an inline bag.
		std::unique_ptr<WASM::ModuleInstance> moduleInstance;
		//! Exported core functions when this handle is an inline bag.
		std::unordered_map<std::string, WASM::Callable> inlineFunctions;
		//! Exported core memories when this handle is an inline bag.
		std::unordered_map<std::string, WASM::LinearMemory*> inlineMemories;

		//! Looks up an exported core function by field name.
		//! \param [in] name The export name to resolve.
		//! \return The callable when the instance or the inline bag exports
		//! `name`, or nullopt when neither does.
		std::optional<WASM::Callable> exportedFunction(
				std::string_view name) const;
		//! Looks up an exported core memory by field name.
		//! \param [in] name The export name to resolve.
		//! \return The linear memory when the instance or the inline bag
		//! exports it, or nullopt when neither does.
		std::optional<WASM::LinearMemory*> exportedMemory(
				std::string_view name) const;
		//! Registers this instance's exports with an import resolver.
		//! \param [in,out] registrar Resolver that receives the exports; it is
		//! owned by the caller and only borrowed here.
		//! \param [in] moduleName Module name the exports are registered under,
		//! as the importing core module spells it.
		void registerExports(WASM::ImportRegistrar& registrar,
				std::string_view moduleName) const;
	};

	ModuleInstantiator& instantiator_; //!< Creates the embedded core modules
	const WASM::Component& component_; //!< Graph and AST being executed
	//! Core instances in definition order, created by processCoreInstance().
	std::vector<std::unique_ptr<CoreInstanceHandle>> coreInstances_;
	ComponentImports imports_; //!< Top-level imports, by import name
	//! Nested components in definition order, owned by the component AST.
	std::vector<const WASM::Component*> nestedComponents_;
	//! One runtime per instantiated nested component, in definition order.
	std::vector<std::unique_ptr<ComponentRuntime>> nestedRuntimes_;
	std::vector<WASM::Callable> coreFuncs_; //!< Core function index space
	std::vector<WASM::LinearMemory*> coreMemories_; //!< Core memory index space
	//! Component function index space, including lifted core functions.
	std::vector<ComponentFunction> componentFuncs_;
	//! `canon lower` closures, allocated per definition and freed in the dtor.
	std::vector<void*> lowerClosures_;
	//! `canon lower` call interfaces, freed in the dtor.
	std::vector<void*> lowerCifs_;
	//! `canon lower` trampoline payloads, freed in the dtor.
	std::vector<void*> lowerData_;
	//! Resource built-in closures, allocated per definition, freed in the dtor.
	std::vector<void*> resourceClosures_;
	//! Resource built-in call interfaces, freed in the dtor.
	std::vector<void*> resourceCifs_;
	//! Resource built-in trampoline payloads, freed in the dtor.
	std::vector<void*> resourceData_;
	//! Handle table; index 0 is a permanently free sentinel, so 0 is never valid.
	std::vector<ResourceSlot> resourceSlots_{ResourceSlot{0, WASM::TypeId::kNone,
			false, true, 0}};
	//! Handles released by freeHandle(), reused before the table grows.
	std::vector<uint32_t> freeResourceSlots_;
	//! Single-agent async state. The allocator/releaser keep async handles in the
	//! same index space as resources (docs/proposals/THREADS.md TD9).
	WASM::ComponentAsyncState async_;
	//! Instance-owned Canonical ABI signatures. Declared after `async_` so the
	//! handle table is constructed first; the table itself only references the
	//! process-lifetime Store.
	mutable WASM::ComponentAbiInstance abi_;
	//! Decoded component `value` definitions, in the value index space. A start
	//! definition refers to these by index. Values are decoded once, after
	//! validation, before the definition walk reaches `start`.
	std::vector<ComponentValue> componentValues_;
	uint32_t borrowScopeSerial_ = 1; //!< Next borrow-scope id handed out
	uint32_t currentBorrowScope_ = 0; //!< Scope currently open; 0 when none
	//! Export name -> index into `componentFuncs_`, for exportedFunction().
	std::unordered_map<std::string, size_t> exportedFuncs_;

	//! Decodes the component values, then walks the definition list once and
	//! builds the graph. Called by the constructor; throws on any violation.
	void build();
	//! Instantiates one `core:instance` definition through the instantiator.
	//! \param [in] instance The definition to instantiate; its module index and
	//! `with` arguments are resolved against the core index spaces built so far.
	void processCoreInstance(const WASM::CoreInstance& instance);
	//! Records one nested component, to be instantiated by a later
	//! component-instance definition.
	//! \param [in] index Index into the component's nested component space.
	void processComponent(uint32_t index);
	//! Instantiates one component-instance definition as a nested runtime.
	//! \param [in] instance The definition; only the `Instantiate` form is
	//! supported, and only function arguments may be substituted.
	void processComponentInstance(const WASM::ComponentInstance& instance);
	//! Applies one alias definition, adding the aliased entity to an index space.
	//! \param [in] alias The alias; only component function export aliases are
	//! supported on this path.
	void processAlias(const WASM::Alias& alias);
	//! Dispatches one canonical definition to its specialised handler.
	//! \param [in] canon The definition; lift/lower and the resource, stream,
	//! async, error-context and thread built-ins each have their own handler.
	void processCanon(const WASM::Canon& canon);
	//! Applies one `resource.*` canonical built-in.
	//! \param [in] canon The definition, whose kind selects new/drop/rep.
	void processResourceCanon(const WASM::Canon& canon);
	//! Applies one `stream.*` canonical built-in.
	//! \param [in] canon The definition, whose kind selects the stream operation.
	void processStreamCanon(const WASM::Canon& canon);
	//! Applies one async built-in (`task.*`, `subtask.*`, `waitable-set.*`).
	//! \param [in] canon The definition, whose kind selects the operation.
	void processAsyncCanon(const WASM::Canon& canon);
	//! Applies one `error-context.*` canonical built-in.
	//! \param [in] canon The definition, whose kind selects new/debug/drop.
	void processErrorContextCanon(const WASM::Canon& canon);
	//! Applies one `thread.*` canonical built-in.
	//! \param [in] canon The definition, whose kind selects the thread operation.
	void processThreadCanon(const WASM::Canon& canon);
	//! Binds one top-level import, from the host map or from a definition of the
	//! same name.
	//! \param [in] importDecl The import declaration to satisfy.
	void processImport(const WASM::ImportDecl& importDecl);
	//! Publishes one top-level export into the export name map.
	//! \param [in] exportDecl The export declaration; only function exports are
	//! recorded for lookup by exportedFunction().
	void processExport(const WASM::ComponentExport& exportDecl);
	//! Runs one start definition, dropping the results it yields.
	//! \param [in] start The start definition: function index, value arguments
	//! and the number of results to discard.
	void processStart(const WASM::Start& start);
	//! Resolves a component value type to its closed canonical form.
	//! \param [in] type The declared value type; a primitive or already
	//! canonical type is returned unchanged.
	//! \return The resolved type, carrying the Store type id when `type` named a
	//! component-level type index.
	//! \throws std::runtime_error when the named type has no closed identity.
	WASM::ComponentValueType resolveValueType(
			const WASM::ComponentValueType& type) const;
	//! Reserves a handle-table slot for a new resource, stream or future.
	//! \return The new handle; a slot released earlier is reused when one is
	//! available. Handle 0 is never returned.
	uint32_t addHandle();
	//! Releases one handle-table slot and returns it to the free list.
	//! \param [in] handle The handle to release; 0, an out-of-range index and a
	//! slot that is not live are ignored, so the call is idempotent.
	void freeHandle(uint32_t handle);

	//! Builds the core-side ABI view for one canonical call.
	//! \param [in] options The canonical options of the call being lowered or
	//! lifted; `kNoIndex` means this call names no memory/realloc/post-return.
	//! \return The memory, realloc, post-return and pointer size to use.
	//! \throws std::runtime_error when a named index is past the end of the
	//! corresponding core index space.
	AbiMemory memoryFor(const WASM::CanonicalAbiOptions& options) const;
	//! Lowers structured component values into flat core values, allocating
	//! strings and lists in the canonical memory.
	//! \param [in] function The signature being called; its `paramTypes` drive
	//! the lowering.
	//! \param [in] values The arguments, in parameter order.
	//! \param [in] memory The core-side ABI view from memoryFor().
	//! \param [out] flat Receives the lowered core values, appended in canonical
	//! ABI order.
	//! \throws std::runtime_error when the argument count does not match.
	void lowerValues(const ComponentFunction& function,
			std::span<const ComponentValue> values,
			const AbiMemory& memory,
			std::vector<WASM::Value>& flat);
	//! Lifts flat core results back into structured component values.
	//! \param [in] function The signature that produced the results; its
	//! `resultTypes` drive the lifting.
	//! \param [in] flat The core results, read in canonical ABI order.
	//! \param [in] memory The core-side ABI view from memoryFor().
	//! \param [out] values Receives one lifted value per result.
	//! \throws std::runtime_error when the result count does not match.
	void liftValues(const ComponentFunction& function,
			std::span<const WASM::Value> flat,
			const AbiMemory& memory,
			std::span<ComponentValue> values);
	//! Calls an instantiated core function through the canonical ABI.
	//! \param [in] callable The core function to call; a null `fnPtr` is
	//! rejected.
	//! \param [in] function The signature the argument and result spans must
	//! have.
	//! \param [in] args The flat core arguments, including the instance context
	//! the runtime prepends.
	//! \param [out] results Receives the flat core results.
	//! \throws std::runtime_error when the argument or result count does not
	//! match the signature, or when the function pointer is null.
	void invokeCore(const WASM::Callable& callable,
			const ComponentFunction& function,
			std::span<const WASM::Value> args,
			std::span<WASM::Value> results) const;
};

} // namespace WASM
WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTRUNTIME_HPP
