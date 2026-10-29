#ifndef WASMCOMPONENTMODEL_HPP
#define WASMCOMPONENTMODEL_HPP

#include "WasmApi.hpp"

/*!
  \file WasmComponentModel.hpp
  \brief The Component Model AST: sections, types and the component graph.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include "WasmType.hpp"
#include "WasmTypeStore.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace WASM {

//! \brief Ordered source definitions
//! Component definitions are acyclic and order-sensitive, but the binary groups
//! them by section. This list restores the order in which index spaces grow.
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

//! One entry of a component's ordered definition list: which kind of definition
//! it is and the index into that kind's own space.
struct ComponentDefinition {
	//! The definition kind (core module, function, value, …).
	ComponentDefinitionKind kind = ComponentDefinitionKind::CoreModule;
	//! Index into the kind's index space.
	uint32_t index = 0;
};

//! \brief Core sorts and indices
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

//! A `core:sort`/index pair naming an entity in a core index space.
struct CoreSortIdx {
	//! Which core index space the index is in.
	CoreSort sort = CoreSort::Func;
	//! Index into that space.
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

//! A component-level sort/index pair; `core` carries the inner core sort when the
//! outer sort is `core`.
struct SortIdx {
	//! Which component index space the index is in.
	ComponentSort sort = ComponentSort::Core;
	//! Set only when sort == Core.
	std::optional<CoreSortIdx> core;
	uint32_t index = 0;
};

//! \brief Core types (the `core:type` section and module-type declarations)
struct CoreExternType {
	enum class Kind : uint8_t { Function, Table, Memory, Global, Tag };
	Kind kind = Kind::Function;
	uint32_t typeIndex = 0; //!< function or tag type index
	TableType table;
	MemoryType memory;
	GlobalType global;
	uint8_t tagAttribute = 0;
};

//! One `core:import` declaration: the module/field it names and its core type.
struct CoreImport {
	//! The importing module's name.
	std::string module;
	//! The field name inside that module.
	std::string name;
	//! The declared type of the imported entity.
	CoreExternType type;
};

//! One `core:export` declaration inside a module type.
struct CoreExportDecl {
	//! The exported field name.
	std::string name;
	//! The declared type of the exported entity.
	CoreExternType type;
};

//! The `outer` alias count/index pair: how many enclosing components to skip and
//! which export of that component to name.
struct CoreOuterAlias {
	//! Number of enclosing components to walk out.
	uint32_t count = 0;
	//! Index into the reached component's export space.
	uint32_t index = 0;
};

//! A `core:type` recursive group: its parser scratch and the interned ids.
struct CoreRecType {
	//! True when the producing `core:rectype` was a `0x4E` recursive group.
	//! A one-member rec group is otherwise indistinguishable from a bare subtype.
	bool isRecGroup = false;
	//! Parser scratch. Validation interns the group and clears this vector, so a
	//! validated component retains only `typeIds` (TYPE_MODEL TM-6 residue).
	mutable std::vector<Subtype> subtypes;
	mutable std::vector<TypeId> typeIds;
};

//! One declaration inside a `core:module` type: an import, a type, an alias or
//! an export, with the payload the kind selects.
struct CoreModuleDecl {
	enum class Kind : uint8_t { Import, Type, Alias, Export };
	Kind kind = Kind::Import;
	CoreImport import;
	CoreRecType type;
	CoreOuterAlias alias;
	CoreExportDecl exportDecl;
};

//! A `core:type` definition: either one recursive group or a module type.
struct CoreTypeDef {
	enum class Kind : uint8_t { RecType, ModuleType };
	Kind kind = Kind::RecType;
	CoreRecType recType;
	std::vector<CoreModuleDecl> moduleDecls;
};

//! \brief Names and attributes
struct Attribute {
	enum class Kind : uint8_t { Implements, VersionSuffix, ExternalId };
	Kind kind = Kind::Implements;
	std::string text;
};

//! A component name's attributes (`implements`, `version-suffix`, `external-id`).
struct NameAttributes {
	uint8_t tag = 0; //!< 0x00, 0x01 (plain/interface) or 0x02 (+ attributes)
	std::string name;
	std::vector<Attribute> attributes;
};

//! A resource type declaration: the core representation and an optional
//! destructor function index.
struct ResourceType {
	ValueType rep; //!< core valtype
	std::optional<uint32_t> destructor;
};

//! A bound on a resource type: `eq` (any resource of the type) or an explicit
//! sub-resource index.
struct TypeBound {
	enum class Kind : uint8_t { Eq, SubResource };
	Kind kind = Kind::Eq;
	uint32_t typeIndex = 0;
};

//! A value type's bound (`eq` or an explicit type).
struct ValueBound {
	enum class Kind : uint8_t { Eq, Type };
	Kind kind = Kind::Eq;
	uint32_t valueIndex = 0;
	ComponentValueType type;
};

//! The type an import or export declares: which sort and the payload that sort
//! uses.
struct ExternType {
	enum class Kind : uint8_t {
		CoreModule, Func, Value, Type, Component, Instance
	};
	Kind kind = Kind::Func;
	uint32_t typeIndex = 0; //!< core-module/func/component/instance
	ValueBound value;
	TypeBound typeBound;
};

//! One `import` declaration of a component or instance type.
struct ImportDecl {
	NameAttributes name;
	ExternType type;
};

//! One type-level `exportdecl`: the name it exports and its ascribed type.
struct ExportDecl {
	NameAttributes name;
	ExternType type;
};

//! A top-level `export` section entry: unlike a type-level `exportdecl`, it
//! names a preceding definition by sortidx and carries an optional ascribed type.
struct ComponentExport {
	NameAttributes name;
	SortIdx sortIndex;
	std::optional<ExternType> type;
};

//! One `alias` declaration: which export (of an instance, of a core instance, or
//! of an outer component) it names.
struct Alias {
	enum class Kind : uint8_t { Export, CoreExport, Outer };
	SortIdx sort;
	Kind kind = Kind::Export;
	uint32_t instanceIndex = 0;
	std::string name;
	uint32_t outerCount = 0;
	uint32_t index = 0;
};

//! One `with` argument of a core-module instantiation: the name it fills and the
//! core instance it takes.
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
	//! Filled by Component::validate() after type substitution: the canonical
	//! identity of the resulting instance type. Exactly one of typeId/schemeId is
	//! set for a validated instance type.
	mutable TypeId typeId{TypeId::kNone};
	mutable SchemeId schemeId{SchemeId::kNone};
};

enum class CanonOptKind : uint8_t {
	Utf8, Utf16, Latin1Utf16, Memory, Realloc, PostReturn, Async, Callback
};

struct CanonOpt {
	CanonOptKind kind = CanonOptKind::Utf8;
	uint32_t index = 0; //!< memory/realloc/post-return/callback
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
	ValueType contextType; //!< context.get/set's core valtype
	std::vector<CanonOpt> opts;
	std::vector<ComponentValueType> results; //!< task.return
	std::optional<bool> async;
	std::optional<bool> shared;
};

//! \brief Type definitions and declarators
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
	//! Defined-value, function and resource payloads live in the shared store.
	//! Component/instance declarations are the remaining inline payloads for
	//! this pass; they keep their order because declaration order is scope.
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

//! \brief Start and value definitions
struct Start {
	uint32_t funcIndex = 0;
	std::vector<uint32_t> args;
	uint32_t resultCount = 0;
};

struct ValueDecl {
	ComponentValueType type;
	uint32_t encodedLength = 0;
	//! The value grammar depends on the type index space; the length-delimited
	//! encoding is retained verbatim, matching how the core parser keeps raw
	//! initialiser expressions and function bodies.
	std::vector<uint8_t> encoded;
};

}

WASMJIT_VISIBLE_END
#endif // WASMCOMPONENTMODEL_HPP