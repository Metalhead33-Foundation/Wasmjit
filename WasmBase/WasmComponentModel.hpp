#ifndef WASMCOMPONENTMODEL_HPP
#define WASMCOMPONENTMODEL_HPP
#include "WasmType.hpp"
#include "WasmTypeStore.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace WASM {

// ── Ordered source definitions ─────────────────────────────────────────────
// Component definitions are acyclic and order-sensitive, but the binary groups
// them by section. This list restores the order in which index spaces grow.
enum class ComponentDefinitionKind : uint8_t {
	CoreModule,
	CoreInstance,
	CoreType,
	Component,
	Instance,
	Alias,
	Type,
	Canon,
	Start,
	Import,
	Export,
	Value
};

struct ComponentDefinition {
	ComponentDefinitionKind kind = ComponentDefinitionKind::CoreModule;
	uint32_t index = 0;
};

// ── Core sorts and indices ──────────────────────────────────────────────────
enum class CoreSort : uint8_t {
	Func = 0x00,
	Table = 0x01,
	Memory = 0x02,
	Global = 0x03,
	Tag = 0x04,
	Type = 0x10,
	Module = 0x11,
	Instance = 0x12
};

struct CoreSortIdx {
	CoreSort sort = CoreSort::Func;
	uint32_t index = 0;
};

enum class ComponentSort : uint8_t {
	Core = 0x00,
	Func = 0x01,
	Value = 0x02,
	Type = 0x03,
	Component = 0x04,
	Instance = 0x05
};

struct SortIdx {
	ComponentSort sort = ComponentSort::Core;
	// Set only when sort == Core.
	std::optional<CoreSortIdx> core;
	uint32_t index = 0;
};

// ── Core types (the `core:type` section and module-type declarations) ──────
struct CoreExternType {
	enum class Kind : uint8_t { Function, Table, Memory, Global, Tag };
	Kind kind = Kind::Function;
	uint32_t typeIndex = 0; // function or tag type index
	TableType table;
	MemoryType memory;
	GlobalType global;
	uint8_t tagAttribute = 0;
};

struct CoreImport {
	std::string module;
	std::string name;
	CoreExternType type;
};

struct CoreExportDecl {
	std::string name;
	CoreExternType type;
};

struct CoreOuterAlias {
	uint32_t count = 0;
	uint32_t index = 0;
};

struct CoreRecType {
	// True when the producing `core:rectype` was a `0x4E` recursive group.
	// A one-member rec group is otherwise indistinguishable from a bare subtype.
	bool isRecGroup = false;
	// Parser scratch. Validation interns the group and clears this vector, so a
	// validated component retains only `typeIds` (TYPE_MODEL TM-6 residue).
	mutable std::vector<Subtype> subtypes;
	mutable std::vector<TypeId> typeIds;
};

struct CoreModuleDecl {
	enum class Kind : uint8_t { Import, Type, Alias, Export };
	Kind kind = Kind::Import;
	CoreImport import;
	CoreRecType type;
	CoreOuterAlias alias;
	CoreExportDecl exportDecl;
};

struct CoreTypeDef {
	enum class Kind : uint8_t { RecType, ModuleType };
	Kind kind = Kind::RecType;
	CoreRecType recType;
	std::vector<CoreModuleDecl> moduleDecls;
};

// ── Names and attributes ────────────────────────────────────────────────────
struct Attribute {
	enum class Kind : uint8_t { Implements, VersionSuffix, ExternalId };
	Kind kind = Kind::Implements;
	std::string text;
};

struct NameAttributes {
	uint8_t tag = 0; // 0x00, 0x01 (plain/interface) or 0x02 (+ attributes)
	std::string name;
	std::vector<Attribute> attributes;
};

struct ResourceType {
	ValueType rep; // core valtype
	std::optional<uint32_t> destructor;
};

struct TypeBound {
	enum class Kind : uint8_t { Eq, SubResource };
	Kind kind = Kind::Eq;
	uint32_t typeIndex = 0;
};

struct ValueBound {
	enum class Kind : uint8_t { Eq, Type };
	Kind kind = Kind::Eq;
	uint32_t valueIndex = 0;
	ComponentValueType type;
};

struct ExternType {
	enum class Kind : uint8_t {
		CoreModule, Func, Value, Type, Component, Instance
	};
	Kind kind = Kind::Func;
	uint32_t typeIndex = 0; // core-module/func/component/instance
	ValueBound value;
	TypeBound typeBound;
};

struct ImportDecl {
	NameAttributes name;
	ExternType type;
};

struct ExportDecl {
	NameAttributes name;
	ExternType type;
};

// A top-level `export` section entry: unlike a type-level `exportdecl`, it
// names a preceding definition by sortidx and carries an optional ascribed type.
struct ComponentExport {
	NameAttributes name;
	SortIdx sortIndex;
	std::optional<ExternType> type;
};

struct Alias {
	enum class Kind : uint8_t { Export, CoreExport, Outer };
	SortIdx sort;
	Kind kind = Kind::Export;
	uint32_t instanceIndex = 0;
	std::string name;
	uint32_t outerCount = 0;
	uint32_t index = 0;
};

struct CoreInstantiateArg {
	std::string name;
	uint32_t instanceIndex = 0;
};

struct CoreInlineExport {
	std::string name;
	CoreSortIdx sortIndex;
};

struct CoreInstance {
	enum class Kind : uint8_t { Instantiate, InlineExports };
	Kind kind = Kind::Instantiate;
	uint32_t moduleIndex = 0;
	std::vector<CoreInstantiateArg> args;
	std::vector<CoreInlineExport> exports;
};

struct InstantiateArg {
	std::string name;
	SortIdx sortIndex;
};

struct InlineExport {
	NameAttributes name;
	SortIdx sortIndex;
};

struct ComponentInstance {
	enum class Kind : uint8_t { Instantiate, InlineExports };
	Kind kind = Kind::Instantiate;
	uint32_t componentIndex = 0;
	std::vector<InstantiateArg> args;
	std::vector<InlineExport> exports;
};

enum class CanonOptKind : uint8_t {
	Utf8, Utf16, Latin1Utf16, Memory, Realloc, PostReturn, Async, Callback
};

struct CanonOpt {
	CanonOptKind kind = CanonOptKind::Utf8;
	uint32_t index = 0; // memory/realloc/post-return/callback
};

struct Canon {
	enum class Kind : uint8_t {
		Lift, Lower, ResourceNew, ResourceDrop, ResourceRep,
		BackpressureInc, BackpressureDec, TaskReturn, TaskCancel,
		ContextGet, ContextSet, SubtaskCancel, SubtaskDrop,
		StreamNew, StreamRead, StreamWrite, StreamCancelRead, StreamCancelWrite,
		StreamDropReadable, StreamDropWritable, StreamForward,
		FutureNew, FutureRead, FutureWrite, FutureCancelRead, FutureCancelWrite,
		FutureDropReadable, FutureDropWritable, FutureForward,
		ErrorContextNew, ErrorContextDebugMessage, ErrorContextDrop,
		WaitableSetNew, WaitableSetWait, WaitableSetPoll, WaitableSetDrop,
		WaitableJoin, ThreadIndex, ThreadNewIndirect, ThreadResumeLater,
		ThreadSuspend, ThreadYield, ThreadSuspendThenResume,
		ThreadYieldThenResume, ThreadSuspendThenPromote,
		ThreadYieldThenPromote, ThreadSpawnRef, ThreadSpawnIndirect,
		ThreadAvailableParallelism
	};
	Kind kind = Kind::Lift;
	uint32_t coreFuncIndex = 0;
	uint32_t funcIndex = 0;
	uint32_t typeIndex = 0;
	uint32_t coreTypeIndex = 0;
	uint32_t tableIndex = 0;
	uint32_t memoryIndex = 0;
	uint32_t contextIndex = 0;
	ValueType contextType; // context.get/set's core valtype
	std::vector<CanonOpt> opts;
	std::vector<ComponentValueType> results; // task.return
	std::optional<bool> async;
	std::optional<bool> shared;
};

// ── Type definitions and declarators ───────────────────────────────────────
struct ComponentTypeDef;
struct ComponentDecl;
struct InstanceDecl;

struct ComponentDecl {
	enum class Kind : uint8_t { Import, CoreType, Type, Alias, Export };
	Kind kind = Kind::Import;
	std::variant<std::monostate, std::unique_ptr<ImportDecl>,
			std::unique_ptr<CoreTypeDef>, std::unique_ptr<ComponentTypeDef>,
			std::unique_ptr<Alias>, std::unique_ptr<ExportDecl>> payload;

	const ImportDecl& import() const { return *std::get<std::unique_ptr<ImportDecl>>(payload); }
	const CoreTypeDef& coreType() const { return *std::get<std::unique_ptr<CoreTypeDef>>(payload); }
	const ComponentTypeDef& type() const { return *std::get<std::unique_ptr<ComponentTypeDef>>(payload); }
	const Alias& alias() const { return *std::get<std::unique_ptr<Alias>>(payload); }
	const ExportDecl& exportDecl() const { return *std::get<std::unique_ptr<ExportDecl>>(payload); }
};

struct InstanceDecl {
	enum class Kind : uint8_t { CoreType, Type, Alias, Export };
	Kind kind = Kind::CoreType;
	std::variant<std::monostate, std::unique_ptr<CoreTypeDef>,
			std::unique_ptr<ComponentTypeDef>, std::unique_ptr<Alias>,
			std::unique_ptr<ExportDecl>> payload;

	const CoreTypeDef& coreType() const { return *std::get<std::unique_ptr<CoreTypeDef>>(payload); }
	const ComponentTypeDef& type() const { return *std::get<std::unique_ptr<ComponentTypeDef>>(payload); }
	const Alias& alias() const { return *std::get<std::unique_ptr<Alias>>(payload); }
	const ExportDecl& exportDecl() const { return *std::get<std::unique_ptr<ExportDecl>>(payload); }
};

struct ComponentTypeDef {
	enum class Kind : uint8_t {
		DefinedValue, Func, Component, Instance, Resource
	};
	Kind kind = Kind::DefinedValue;
	// Defined-value, function and resource payloads live in the shared store.
	// Component/instance declarations are the remaining inline payloads for
	// this pass; they keep their order because declaration order is scope.
	TypeId id{TypeId::kNone};
	std::variant<std::monostate, std::vector<ComponentDecl>,
			std::vector<InstanceDecl>, ResourceType> declarations;

	const std::vector<ComponentDecl>& componentDecls() const
	{
		return std::get<std::vector<ComponentDecl>>(declarations);
	}
	const std::vector<InstanceDecl>& instanceDecls() const
	{
		return std::get<std::vector<InstanceDecl>>(declarations);
	}
	const ResourceType& resource() const
	{
		return std::get<ResourceType>(declarations);
	}
};

// ── Start and value definitions ────────────────────────────────────────────
struct Start {
	uint32_t funcIndex = 0;
	std::vector<uint32_t> args;
	uint32_t resultCount = 0;
};

struct ValueDecl {
	ComponentValueType type;
	uint32_t encodedLength = 0;
	// The value grammar depends on the type index space; the length-delimited
	// encoding is retained verbatim, matching how the core parser keeps raw
	// initialiser expressions and function bodies.
	std::vector<uint8_t> encoded;
};

}

#endif // WASMCOMPONENTMODEL_HPP