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

//! A component-level sort: which index space a `sortidx` names.
enum class ComponentSort : uint8_t {
	Core = 0x00, //!< Nested core sort (see CoreSortIdx)
	Func = 0x01, //!< Component function
	Value = 0x02, //!< Component value
	Type = 0x03, //!< Component type
	Component = 0x04, //!< Nested component
	Instance = 0x05 //!< Component instance
};

//! A component-level sort/index pair; `core` carries the inner core sort when the
//! outer sort is `core`.
struct SortIdx {
	//! Which component index space the index is in.
	ComponentSort sort = ComponentSort::Core;
	//! Set only when sort == Core.
	std::optional<CoreSortIdx> core;
	uint32_t index = 0; //!< Index into the selected space
};

//! \brief Core types (the `core:type` section and module-type declarations)
struct CoreExternType {
	//! Which core extern form this declaration carries.
	enum class Kind : uint8_t {
		Function, //!< Function type index
		Table, //!< Table type
		Memory, //!< Memory type
		Global, //!< Global type
		Tag //!< Tag type index plus attribute
	};
	Kind kind = Kind::Function; //!< Discriminant for the payload below
	uint32_t typeIndex = 0; //!< function or tag type index
	TableType table; //!< Valid when kind == Table
	MemoryType memory; //!< Valid when kind == Memory
	GlobalType global; //!< Valid when kind == Global
	uint8_t tagAttribute = 0; //!< Tag attribute byte when kind == Tag
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
	mutable std::vector<TypeId> typeIds; //!< Interned ids after validation
};

//! One declaration inside a `core:module` type: an import, a type, an alias or
//! an export, with the payload the kind selects.
struct CoreModuleDecl {
	//! Which form this module-type declaration takes.
	enum class Kind : uint8_t {
		Import, //!< Core import
		Type, //!< Nested core type
		Alias, //!< Outer alias
		Export //!< Core export
	};
	Kind kind = Kind::Import; //!< Discriminant for the payload below
	CoreImport import; //!< Valid when kind == Import
	CoreRecType type; //!< Valid when kind == Type
	CoreOuterAlias alias; //!< Valid when kind == Alias
	CoreExportDecl exportDecl; //!< Valid when kind == Export
};

//! A `core:type` definition: either one recursive group or a module type.
struct CoreTypeDef {
	//! Which form this core type definition takes.
	enum class Kind : uint8_t {
		RecType, //!< Recursive group / subtype
		ModuleType //!< Module type with declarations
	};
	Kind kind = Kind::RecType; //!< Discriminant for the payload below
	CoreRecType recType; //!< Valid when kind == RecType
	std::vector<CoreModuleDecl> moduleDecls; //!< Valid when kind == ModuleType
};

//! \brief Names and attributes
struct Attribute {
	//! Which name attribute this entry is.
	enum class Kind : uint8_t {
		Implements, //!< `implements` attribute
		VersionSuffix, //!< `version-suffix` attribute
		ExternalId //!< `external-id` attribute
	};
	Kind kind = Kind::Implements; //!< Attribute kind
	std::string text; //!< Attribute text payload
};

//! A component name's attributes (`implements`, `version-suffix`, `external-id`).
struct NameAttributes {
	uint8_t tag = 0; //!< 0x00, 0x01 (plain/interface) or 0x02 (+ attributes)
	std::string name; //!< The name string
	std::vector<Attribute> attributes; //!< Extra attributes when tag == 0x02
};

//! A resource type declaration: the core representation and an optional
//! destructor function index.
struct ResourceType {
	ValueType rep; //!< core valtype
	std::optional<uint32_t> destructor; //!< Optional destructor core-func index
};

//! A bound on a resource type: `eq` (any resource of the type) or an explicit
//! sub-resource index.
struct TypeBound {
	//! Which form of resource bound this is.
	enum class Kind : uint8_t {
		Eq, //!< Any resource of the named type
		SubResource //!< Explicit sub-resource index
	};
	Kind kind = Kind::Eq; //!< Bound kind
	uint32_t typeIndex = 0; //!< Resource type index
};

//! A value type's bound (`eq` or an explicit type).
struct ValueBound {
	//! Which form of value bound this is.
	enum class Kind : uint8_t {
		Eq, //!< Equality with a prior value index
		Type //!< Explicit component value type
	};
	Kind kind = Kind::Eq; //!< Bound kind
	uint32_t valueIndex = 0; //!< Prior value index when kind == Eq
	ComponentValueType type; //!< Explicit type when kind == Type
};

//! The type an import or export declares: which sort and the payload that sort
//! uses.
struct ExternType {
	//! Which sort this extern type declares.
	enum class Kind : uint8_t {
		CoreModule, //!< Core module type index
		Func, //!< Function type index
		Value, //!< Value bound
		Type, //!< Type bound
		Component, //!< Component type index
		Instance //!< Instance type index
	};
	Kind kind = Kind::Func; //!< Discriminant for the payload below
	uint32_t typeIndex = 0; //!< core-module/func/component/instance
	ValueBound value; //!< Valid when kind == Value
	TypeBound typeBound; //!< Valid when kind == Type
};

//! One `import` declaration of a component or instance type.
struct ImportDecl {
	NameAttributes name; //!< Import name and attributes
	ExternType type; //!< Declared type of the import
};

//! One type-level `exportdecl`: the name it exports and its ascribed type.
struct ExportDecl {
	NameAttributes name; //!< Export name and attributes
	ExternType type; //!< Ascribed type of the export
};

//! A top-level `export` section entry: unlike a type-level `exportdecl`, it
//! names a preceding definition by sortidx and carries an optional ascribed type.
struct ComponentExport {
	NameAttributes name; //!< Export name and attributes
	SortIdx sortIndex; //!< Preceding definition being exported
	std::optional<ExternType> type; //!< Optional ascribed type
};

//! One `alias` declaration: which export (of an instance, of a core instance, or
//! of an outer component) it names.
struct Alias {
	//! Which form of alias this is.
	enum class Kind : uint8_t {
		Export, //!< Export of a component instance
		CoreExport, //!< Export of a core instance
		Outer //!< Outer-component alias
	};
	SortIdx sort; //!< Sort of the aliased entity
	Kind kind = Kind::Export; //!< Alias form
	uint32_t instanceIndex = 0; //!< Instance being aliased (Export/CoreExport)
	std::string name; //!< Export name (Export/CoreExport)
	uint32_t outerCount = 0; //!< Enclosing components to skip (Outer)
	uint32_t index = 0; //!< Index in the reached space (Outer)
};

//! One `with` argument of a core-module instantiation: the name it fills and the
//! core instance it takes.
struct CoreInstantiateArg {
	std::string name; //!< Import name being satisfied
	uint32_t instanceIndex = 0; //!< Core instance supplying the import
};

//! One inline core export of a `core:instance` bag.
struct CoreInlineExport {
	std::string name; //!< Exported field name
	CoreSortIdx sortIndex; //!< Core entity being exported
};

//! A `core:instance` definition: instantiate a core module, or an inline bag of
//! core exports.
struct CoreInstance {
	//! Which form this core instance takes.
	enum class Kind : uint8_t {
		Instantiate, //!< Instantiate a core module
		InlineExports //!< Inline bag of core exports
	};
	Kind kind = Kind::Instantiate; //!< Discriminant for the payload below
	uint32_t moduleIndex = 0; //!< Core module when kind == Instantiate
	std::vector<CoreInstantiateArg> args; //!< Instantiation arguments
	std::vector<CoreInlineExport> exports; //!< Inline exports
};

//! One `with` argument of a component instantiation.
struct InstantiateArg {
	std::string name; //!< Import name being satisfied
	SortIdx sortIndex; //!< Entity supplying the import
};

//! One inline export of a component-instance bag.
struct InlineExport {
	NameAttributes name; //!< Exported name and attributes
	SortIdx sortIndex; //!< Entity being exported
};

//! A component-instance definition: instantiate a nested component, or an
//! inline bag of exports.
struct ComponentInstance {
	//! Which form this component instance takes.
	enum class Kind : uint8_t {
		Instantiate, //!< Instantiate a nested component
		InlineExports //!< Inline bag of exports
	};
	Kind kind = Kind::Instantiate; //!< Discriminant for the payload below
	uint32_t componentIndex = 0; //!< Nested component when Instantiate
	std::vector<InstantiateArg> args; //!< Instantiation arguments
	std::vector<InlineExport> exports; //!< Inline exports
	//! Filled by Component::validate() after type substitution: the canonical
	//! identity of the resulting instance type. Exactly one of typeId/schemeId is
	//! set for a validated instance type.
	mutable TypeId typeId{TypeId::kNone};
	mutable SchemeId schemeId{SchemeId::kNone};
};

//! A Canonical ABI option tag carried by a `canon` definition.
enum class CanonOptKind : uint8_t {
	Utf8, //!< UTF-8 string encoding
	Utf16, //!< UTF-16 string encoding
	Latin1Utf16, //!< latin1+utf16 string encoding
	Memory, //!< Memory option (index follows)
	Realloc, //!< Realloc option (index follows)
	PostReturn, //!< Post-return option (index follows)
	Async, //!< Async lifting/lowering
	Callback //!< Callback option (index follows)
};

//! One Canonical ABI option and its optional index payload.
struct CanonOpt {
	CanonOptKind kind = CanonOptKind::Utf8; //!< Option kind
	uint32_t index = 0; //!< memory/realloc/post-return/callback
};

//! One `canon` definition: lift/lower or a built-in adaptation.
struct Canon {
	//! Which Canonical ABI adaptation this definition is.
	enum class Kind : uint8_t {
		Lift, //!< Lift a core function
		Lower, //!< Lower a component function
		ResourceNew, //!< resource.new
		ResourceDrop, //!< resource.drop
		ResourceRep, //!< resource.rep
		BackpressureInc, //!< backpressure.inc
		BackpressureDec, //!< backpressure.dec
		TaskReturn, //!< task.return
		TaskCancel, //!< task.cancel
		ContextGet, //!< context.get
		ContextSet, //!< context.set
		SubtaskCancel, //!< subtask.cancel
		SubtaskDrop, //!< subtask.drop
		StreamNew, //!< stream.new
		StreamRead, //!< stream.read
		StreamWrite, //!< stream.write
		StreamCancelRead, //!< stream.cancel-read
		StreamCancelWrite, //!< stream.cancel-write
		StreamDropReadable, //!< stream.drop-readable
		StreamDropWritable, //!< stream.drop-writable
		StreamForward, //!< stream.forward
		FutureNew, //!< future.new
		FutureRead, //!< future.read
		FutureWrite, //!< future.write
		FutureCancelRead, //!< future.cancel-read
		FutureCancelWrite, //!< future.cancel-write
		FutureDropReadable, //!< future.drop-readable
		FutureDropWritable, //!< future.drop-writable
		FutureForward, //!< future.forward
		ErrorContextNew, //!< error-context.new
		ErrorContextDebugMessage, //!< error-context.debug-message
		ErrorContextDrop, //!< error-context.drop
		WaitableSetNew, //!< waitable-set.new
		WaitableSetWait, //!< waitable-set.wait
		WaitableSetPoll, //!< waitable-set.poll
		WaitableSetDrop, //!< waitable-set.drop
		WaitableJoin, //!< waitable.join
		ThreadIndex, //!< thread.index
		ThreadNewIndirect, //!< thread.new-indirect
		ThreadResumeLater, //!< thread.resume-later
		ThreadSuspend, //!< thread.suspend
		ThreadYield, //!< thread.yield
		ThreadSuspendThenResume, //!< thread.suspend-then-resume
		ThreadYieldThenResume, //!< thread.yield-then-resume
		ThreadSuspendThenPromote, //!< thread.suspend-then-promote
		ThreadYieldThenPromote, //!< thread.yield-then-promote
		ThreadSpawnRef, //!< thread.spawn-ref
		ThreadSpawnIndirect, //!< thread.spawn-indirect
		ThreadAvailableParallelism //!< thread.available-parallelism
	};
	Kind kind = Kind::Lift; //!< Adaptation kind
	uint32_t coreFuncIndex = 0; //!< Core function (lift/lower/built-ins)
	uint32_t funcIndex = 0; //!< Component function (lower)
	uint32_t typeIndex = 0; //!< Component type index
	uint32_t coreTypeIndex = 0; //!< Core type index
	uint32_t tableIndex = 0; //!< Table index (thread built-ins)
	uint32_t memoryIndex = 0; //!< Memory index when named directly
	uint32_t contextIndex = 0; //!< Context slot index
	ValueType contextType; //!< context.get/set's core valtype
	std::vector<CanonOpt> opts; //!< Canonical ABI options
	std::vector<ComponentValueType> results; //!< task.return
	std::optional<bool> async; //!< Async flag when present
	std::optional<bool> shared; //!< Shared flag when present
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