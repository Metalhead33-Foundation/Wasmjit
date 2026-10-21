#include "WasmValidator.hpp"
#include "WasmStore.hpp"
#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include <limits>
#include <unordered_set>

namespace WASM {

Validator::Validator(const Module& module)
	: module(module)
{
	totalFunctions = module.importFunctions.size() + module.internalFunctionTypeIndices.size();
	totalTables = module.importTables.size() + module.tables.size();
	totalMemories = module.importMemories.size() + module.memories.size();
	totalGlobals = module.importGlobals.size() + module.globals.size();
	totalTags = module.importTags.size() + module.tags.size();
}

void Validator::fail(const std::string& message) const
{
	if (currentBody == kNoBody)
		throw ValidationError(message);

	std::string prefix = "function " + std::to_string(currentBody);
	// `currentBody` indexes the internal function bodies; the name section is
	// keyed by the global function index (imports first).
	const std::size_t globalIndex = currentBody + module.importFunctions.size();
	if (const std::string* name = module.findName(NameKind::Function, globalIndex))
		prefix += " ($" + *name + ")";
	prefix += " at instruction " + std::to_string(instructionOffset) + ": ";
	throw ValidationError(prefix + message);
}

std::string Validator::describeLocal(uint32_t idx) const
{
	if (currentBody == kNoBody)
		return std::to_string(idx);
	const std::size_t globalIndex = currentBody + module.importFunctions.size();
	return module.describeIndex(NameKind::Local, static_cast<uint32_t>(globalIndex), idx);
}

FuncType Validator::funcTypeAt(uint32_t typeIdx) const
{
	if (typeIdx >= module.typeCount())
		fail("unknown type " + module.describeIndex(NameKind::Type, typeIdx));
	if (module.typeView(typeIdx).kind() != CanonTypeKind::Func)
		fail("type " + module.describeIndex(NameKind::Type, typeIdx) + " is not a function type");
	return module.funcTypeAt(typeIdx);
}

void Validator::requireFunctionIndex(uint32_t idx) const
{
	if (idx >= totalFunctions)
		fail("unknown function " + module.describeIndex(NameKind::Function, idx));
}

void Validator::requireTableIndex(uint32_t idx) const
{
	if (idx >= totalTables)
		fail("unknown table " + module.describeIndex(NameKind::Table, idx));
}

void Validator::requireMemoryIndex(uint32_t idx) const
{
	if (idx >= totalMemories)
		fail("unknown memory " + module.describeIndex(NameKind::Memory, idx));
}

void Validator::requireGlobalIndex(uint32_t idx) const
{
	if (idx >= totalGlobals)
		fail("unknown global " + module.describeIndex(NameKind::Global, idx));
}

void Validator::requireTagIndex(uint32_t idx) const
{
	if (idx >= totalTags)
		fail("unknown tag " + module.describeIndex(NameKind::Tag, idx));
}

void Validator::requireLocalIndex(uint32_t idx) const
{
	if (idx >= locals.size())
		fail("unknown local " + describeLocal(idx));
}

void Validator::requireElementIndex(uint32_t idx) const
{
	if (idx >= module.elementSegments.size())
		fail("unknown element segment " + module.describeIndex(NameKind::Element, idx));
}

void Validator::requireDataIndex(uint32_t idx) const
{
	if (idx >= module.dataSegments.size())
		fail("unknown data segment " + module.describeIndex(NameKind::Data, idx));
}

bool Validator::globalIsMutable(uint32_t idx) const
{
	if (idx < module.importGlobals.size())
		return module.importGlobals[idx].global.isMutable;
	return module.globals[idx - module.importGlobals.size()].type.isMutable;
}

bool Validator::tableIsFuncRef(uint32_t idx) const
{
	const ValueType* element = nullptr;
	if (idx < module.importTables.size())
		element = &module.importTables[idx].table.elementType;
	else
		element = &module.tables[idx - module.importTables.size()].elementType;

	switch (element->opcode) {
	case ValueTypeCode::FuncRef:
	case ValueTypeCode::NullFuncRef:
		return true;
	case ValueTypeCode::Ref:
	case ValueTypeCode::RefNull:
		if (element->heapType >= 0) {
			const auto typeIdx = static_cast<uint32_t>(element->heapType);
			return typeIdx < module.typeCount()
					&& module.typeView(typeIdx).kind() == CanonTypeKind::Func;
		}
		return element->heapType == static_cast<int32_t>(ValueTypeCode::Func);
	default:
		return false;
	}
}

bool Validator::memoryIs64(uint32_t idx) const
{
	if (idx < module.importMemories.size())
		return module.importMemories[idx].memory.indexType == MemoryIndexType::I64;
	return module.memories[idx - module.importMemories.size()].indexType == MemoryIndexType::I64;
}

bool Validator::memoryIsShared(uint32_t idx) const
{
	const Limits& limits = idx < module.importMemories.size()
			? module.importMemories[idx].memory.limits
			: module.memories[idx - module.importMemories.size()].limits;
	return (limits.flags & 0x02) != 0;
}

void Validator::checkLimits(const Limits& limits, bool is64, bool isMemory,
							const char* what, std::size_t index) const
{
	const bool hasMax = (limits.flags & 0x01) != 0;
	const bool shared = isMemory && (limits.flags & 0x02) != 0;
	const std::string label = std::string(what) + " " + std::to_string(index);

	if (hasMax && limits.initial > *limits.maximum)
		fail(label + ": size minimum must not be greater than maximum");

	const uint64_t cap = isMemory
			? (is64 ? (1ull << 48) : 65536ull)
			: (is64 ? std::numeric_limits<uint64_t>::max() : 0xFFFFFFFFull);
	if (limits.initial > cap)
		fail(label + ": size minimum is out of range");
	if (hasMax && *limits.maximum > cap)
		fail(label + ": size maximum is out of range");
	if (shared && !hasMax)
		fail(label + ": shared memory must declare a maximum");
}

void Validator::requireMemoryAccess(const MemArg& arg, uint32_t naturalLog2) const
{
	requireMemoryIndex(arg.memidx);
	if (arg.align > naturalLog2)
		fail("alignment must not be larger than natural");
	if (!memoryIs64(arg.memidx) && arg.offset > 0xFFFFFFFFull)
		fail("offset out of range");
}

void Validator::requireLane(uint8_t lane, uint32_t laneCount, const char* what) const
{
	if (lane >= laneCount)
		fail(std::string(what) + ": invalid lane index " + std::to_string(lane));
}

Validator::ControlFrame& Validator::frame()
{
	if (controlStack.empty())
		fail("instruction outside a function body");
	return controlStack.back();
}

std::vector<ValueType> Validator::paramTypes(const FuncType& types) const
{
	std::vector<ValueType> out;
	for (const StorageType& type : types.params)
		out.push_back(type.val);
	return out;
}

std::vector<ValueType> Validator::resultTypes(const FuncType& types) const
{
	std::vector<ValueType> out;
	for (const StorageType& type : types.results)
		out.push_back(type.val);
	return out;
}

std::vector<ValueType> Validator::blockParams(const BlockType& blockType) const
{
	if (blockType.isTypeIndex)
		return paramTypes(funcTypeAt(blockType.typeIndex));
	if (blockType.heapType >= 0
			&& static_cast<uint32_t>(blockType.heapType) >= module.typeCount())
		fail("block type: unknown type "
			 + module.describeIndex(NameKind::Type, static_cast<uint32_t>(blockType.heapType)));
	// A value-typed block type is a single *result*; it never has parameters.
	return {};
}

std::vector<ValueType> Validator::blockResults(const BlockType& blockType) const
{
	if (blockType.isTypeIndex)
		return resultTypes(funcTypeAt(blockType.typeIndex));
	if (blockType.isVoid())
		return {};
	if (blockType.heapType >= 0
			&& static_cast<uint32_t>(blockType.heapType) >= module.typeCount())
		fail("block type: unknown type "
			 + module.describeIndex(NameKind::Type, static_cast<uint32_t>(blockType.heapType)));
	return {ValueType{blockType.valType, blockType.heapType}};
}

void Validator::pushValue(const ValueType& type)
{
	if (!typeTainted)
		typeStack.push_back(type);
}

std::optional<ValueType> Validator::popAny()
{
	if (typeTainted)
		return std::nullopt;
	ControlFrame& current = frame();
	if (typeStack.size() == current.height && current.unreachable)
		return std::nullopt;
	if (typeStack.size() <= current.height)
		fail("type mismatch: operand stack underflow");
	ValueType type = typeStack.back();
	typeStack.pop_back();
	return type;
}

void Validator::popExpect(ValueTypeCode code)
{
	const auto actual = popAny();
	if (actual && actual->opcode != code)
		fail("type mismatch: unexpected operand type");
}

bool Validator::assignable(const ValueType& actual, const ValueType& expected) const
{
	if (actual.opcode == expected.opcode && actual.heapType == expected.heapType)
		return true;
	const auto isRef = [](ValueTypeCode code) {
		return code == ValueTypeCode::FuncRef || code == ValueTypeCode::ExternRef
				|| code == ValueTypeCode::AnyRef || code == ValueTypeCode::EqRef
				|| code == ValueTypeCode::I31Ref || code == ValueTypeCode::StructRef
				|| code == ValueTypeCode::ArrayRef || code == ValueTypeCode::ExnRef
				|| code == ValueTypeCode::NullFuncRef || code == ValueTypeCode::NullExternRef
				|| code == ValueTypeCode::NullExnRef || code == ValueTypeCode::NullRef
				|| code == ValueTypeCode::StringRef || code == ValueTypeCode::StringViewWtf8
				|| code == ValueTypeCode::StringViewWtf16 || code == ValueTypeCode::StringViewIter
				|| code == ValueTypeCode::Ref || code == ValueTypeCode::RefNull;
	};
	if (!isRef(actual.opcode) || !isRef(expected.opcode))
		return false;
	return refSubtype(actual, expected);
}

bool Validator::isDefaultable(const ValueType& type) const
{
	switch (type.opcode) {
	case ValueTypeCode::I32:
	case ValueTypeCode::I64:
	case ValueTypeCode::F32:
	case ValueTypeCode::F64:
	case ValueTypeCode::V128:
	case ValueTypeCode::FuncRef:
	case ValueTypeCode::ExternRef:
	case ValueTypeCode::AnyRef:
	case ValueTypeCode::EqRef:
	case ValueTypeCode::I31Ref:
	case ValueTypeCode::StructRef:
	case ValueTypeCode::ArrayRef:
	case ValueTypeCode::ExnRef:
	case ValueTypeCode::NullRef:
	case ValueTypeCode::NullFuncRef:
	case ValueTypeCode::NullExternRef:
	case ValueTypeCode::NullExnRef:
	case ValueTypeCode::StringRef:
	case ValueTypeCode::StringViewWtf8:
	case ValueTypeCode::StringViewWtf16:
	case ValueTypeCode::StringViewIter:
	case ValueTypeCode::RefNull:
		return true;
	case ValueTypeCode::Ref:
		return false;
	default:
		// A type this validator does not model: never reject a valid module.
		return true;
	}
}

bool Validator::refSubtype(const ValueType& actual, const ValueType& expected) const
{
	enum class Kind { Func, Extern, Exn, Any, Eq, I31, Struct, Array,
					  String, StringViewWtf8, StringViewWtf16, StringViewIter,
					  None, NoFunc, NoExtern, NoExn,
					  ConcreteFunc, ConcreteStruct, ConcreteArray, Other };
	struct Info { bool isRef = false; bool nullable = false; Kind kind = Kind::Other; uint32_t idx = 0; };

	const auto info = [this](const ValueType& type) {
		Info out;
		switch (type.opcode) {
		case ValueTypeCode::FuncRef:      out = {true, true, Kind::Func, 0}; break;
		case ValueTypeCode::ExternRef:    out = {true, true, Kind::Extern, 0}; break;
		case ValueTypeCode::ExnRef:       out = {true, true, Kind::Exn, 0}; break;
		case ValueTypeCode::AnyRef:       out = {true, true, Kind::Any, 0}; break;
		case ValueTypeCode::EqRef:        out = {true, true, Kind::Eq, 0}; break;
		case ValueTypeCode::I31Ref:       out = {true, true, Kind::I31, 0}; break;
		case ValueTypeCode::StructRef:    out = {true, true, Kind::Struct, 0}; break;
		case ValueTypeCode::ArrayRef:     out = {true, true, Kind::Array, 0}; break;
		case ValueTypeCode::StringRef:       out = {true, true, Kind::String, 0}; break;
		case ValueTypeCode::StringViewWtf8:  out = {true, true, Kind::StringViewWtf8, 0}; break;
		case ValueTypeCode::StringViewWtf16: out = {true, true, Kind::StringViewWtf16, 0}; break;
		case ValueTypeCode::StringViewIter:  out = {true, true, Kind::StringViewIter, 0}; break;
		case ValueTypeCode::NullRef:      out = {true, true, Kind::None, 0}; break;
		case ValueTypeCode::NullFuncRef:  out = {true, true, Kind::NoFunc, 0}; break;
		case ValueTypeCode::NullExternRef:out = {true, true, Kind::NoExtern, 0}; break;
		case ValueTypeCode::NullExnRef:   out = {true, true, Kind::NoExn, 0}; break;
		case ValueTypeCode::Ref:
		case ValueTypeCode::RefNull: {
			out.isRef = true;
			out.nullable = (type.opcode == ValueTypeCode::RefNull);
			if (type.heapType >= 0) {
				const auto idx = static_cast<uint32_t>(type.heapType);
				out.idx = idx;
				if (idx >= module.typeCount()) {
					out.kind = Kind::Other;
					break;
				}
				switch (module.typeView(idx).kind()) {
				case CanonTypeKind::Func:   out.kind = Kind::ConcreteFunc; break;
				case CanonTypeKind::Struct: out.kind = Kind::ConcreteStruct; break;
				case CanonTypeKind::Array:  out.kind = Kind::ConcreteArray; break;
				case CanonTypeKind::Import: {
					const CanonHeapType bound = module.typeView(idx).importBound();
					switch (bound.abstract) {
					case AbstractHeapType::Func:   out.kind = Kind::ConcreteFunc; break;
					case AbstractHeapType::Struct: out.kind = Kind::ConcreteStruct; break;
					case AbstractHeapType::Array:  out.kind = Kind::ConcreteArray; break;
					case AbstractHeapType::Extern: out.kind = Kind::Extern; break;
					default:                       out.kind = Kind::Any; break;
					}
					break;
				}
				}
			} else {
				switch (static_cast<ValueTypeCode>(type.heapType)) {
				case ValueTypeCode::Func:
				case ValueTypeCode::FuncRef:   out.kind = Kind::Func; break;
				case ValueTypeCode::ExternRef: out.kind = Kind::Extern; break;
				case ValueTypeCode::ExnRef:     out.kind = Kind::Exn; break;
				case ValueTypeCode::AnyRef:     out.kind = Kind::Any; break;
				case ValueTypeCode::EqRef:      out.kind = Kind::Eq; break;
				case ValueTypeCode::I31Ref:     out.kind = Kind::I31; break;
				case ValueTypeCode::StructRef:  out.kind = Kind::Struct; break;
				case ValueTypeCode::ArrayRef:   out.kind = Kind::Array; break;
				case ValueTypeCode::NullRef:    out.kind = Kind::None; break;
				case ValueTypeCode::NullFuncRef:out.kind = Kind::NoFunc; break;
				case ValueTypeCode::NullExternRef:out.kind = Kind::NoExtern; break;
				case ValueTypeCode::NullExnRef: out.kind = Kind::NoExn; break;
				default:                        out.kind = Kind::Other; break;
				}
			}
			break;
		}
		default:
			break;
		}
		return out;
	};

	const Info a = info(actual);
	const Info e = info(expected);
	if (!a.isRef || !e.isRef)
		return false;
	// A nullable value cannot satisfy a non-null requirement.
	if (a.nullable && !e.nullable)
		return false;

	// Bottom types (nullref/nofunc/noextern/noexn) are subtypes of their hierarchy.
	if (a.kind == Kind::None)
		return e.kind == Kind::None || e.kind == Kind::Any || e.kind == Kind::Eq
				|| e.kind == Kind::I31 || e.kind == Kind::Struct || e.kind == Kind::Array
				|| e.kind == Kind::ConcreteStruct || e.kind == Kind::ConcreteArray;
	if (a.kind == Kind::NoFunc)
		return e.kind == Kind::NoFunc || e.kind == Kind::Func || e.kind == Kind::ConcreteFunc;
	if (a.kind == Kind::NoExtern)
		return e.kind == Kind::NoExtern || e.kind == Kind::Extern;
	if (a.kind == Kind::NoExn)
		return e.kind == Kind::NoExn || e.kind == Kind::Exn;

	// A concrete expected type only accepts a concrete actual (with subtyping).
	if (e.kind == Kind::ConcreteFunc || e.kind == Kind::ConcreteStruct || e.kind == Kind::ConcreteArray) {
		if (a.kind == Kind::ConcreteFunc || a.kind == Kind::ConcreteStruct || a.kind == Kind::ConcreteArray)
			return module.isSubtype(static_cast<TypeIdx>(a.idx), static_cast<TypeIdx>(e.idx));
		return false;
	}

	// A concrete actual is a subtype of the matching abstract top(s).
	if (a.kind == Kind::ConcreteFunc)
		return e.kind == Kind::Func;
	if (a.kind == Kind::ConcreteStruct)
		return e.kind == Kind::Struct || e.kind == Kind::Eq || e.kind == Kind::Any;
	if (a.kind == Kind::ConcreteArray)
		return e.kind == Kind::Array || e.kind == Kind::Eq || e.kind == Kind::Any;

	switch (e.kind) {
	case Kind::Any:
		return a.kind == Kind::Any || a.kind == Kind::Eq || a.kind == Kind::I31
				|| a.kind == Kind::Struct || a.kind == Kind::Array;
	case Kind::Eq:
		return a.kind == Kind::Eq || a.kind == Kind::I31
				|| a.kind == Kind::Struct || a.kind == Kind::Array;
	case Kind::I31:    return a.kind == Kind::I31;
	case Kind::Struct: return a.kind == Kind::Struct;
	case Kind::Array:  return a.kind == Kind::Array;
	case Kind::Func:   return a.kind == Kind::Func;
	case Kind::Extern: return a.kind == Kind::Extern;
	case Kind::Exn:    return a.kind == Kind::Exn;
	default:           return a.kind == e.kind;
	}
}

void Validator::requireWellFormedType(const ValueType& type, const std::string& where) const
{
	if ((type.opcode == ValueTypeCode::Ref || type.opcode == ValueTypeCode::RefNull)
			&& type.heapType >= 0
			&& static_cast<uint32_t>(type.heapType) >= module.typeCount()) {
		fail(where + ": unknown type " + std::to_string(type.heapType));
	}
}

void Validator::popExpect(const ValueType& expected)
{
	const auto actual = popAny();
	if (actual && !assignable(*actual, expected))
		fail("type mismatch: unexpected operand type");
}

void Validator::pushTypes(const std::vector<ValueType>& types)
{
	for (const ValueType& type : types)
		typeStack.push_back(type);
}

void Validator::popTypes(const std::vector<ValueType>& types)
{
	for (auto it = types.rbegin(); it != types.rend(); ++it)
		popExpect(*it);
}

void Validator::checkTailCallResults(const FuncType& callee)
{
	if (currentFuncType == nullptr)
		return;
	const std::vector<ValueType> expected = resultTypes(*currentFuncType);
	const std::vector<ValueType> actual = resultTypes(callee);
	if (expected.size() != actual.size())
		fail("type mismatch: tail call result arity");
	for (std::size_t i = 0; i < expected.size(); ++i) {
		if (!assignable(actual[i], expected[i]))
			fail("type mismatch: tail call result type");
	}
}

void Validator::setUnreachable()
{
	ControlFrame& current = frame();
	current.unreachable = true;
	typeStack.resize(current.height);
}

void Validator::beginBlock(const BlockType& blockType, bool isLoop, bool isIf)
{
	const std::vector<ValueType> params = blockParams(blockType);
	const std::vector<ValueType> results = blockResults(blockType);
	popTypes(params);
	const std::size_t height = typeStack.size();
	pushTypes(params);
	controlStack.push_back(ControlFrame{params, results, height, isLoop, isIf, false, false, {}, {}});
	// A block inherits the definite-assignment state of its parent; stores made
	// inside it do not escape at `end`.
	controlStack.back().initialized = controlStack[controlStack.size() - 2].initialized;
	controlStack.back().entryInitialized = controlStack.back().initialized;
}

void Validator::endBlock(bool isElse)
{
	if (controlStack.empty())
		fail("end without a matching block");
	ControlFrame& current = controlStack.back();
	if (isElse) {
		if (!current.isIf || current.elseSeen)
			fail("else without a matching if");
		popTypes(current.endTypes);
		if (!typeTainted && typeStack.size() != current.height)
			fail("type mismatch: values remaining at else");
		typeStack.resize(current.height);
		pushTypes(current.startTypes);
		current.unreachable = false;
		current.elseSeen = true;
		// The else branch starts from the same definite-assignment state as the
		// then branch; stores in the then branch must not leak into it.
		current.initialized = current.entryInitialized;
		return;
	}
	popTypes(current.endTypes);
	if (!typeTainted && typeStack.size() != current.height)
		fail("type mismatch: values remaining at end");
	if (current.isIf && !current.elseSeen) {
		if (current.startTypes.size() != current.endTypes.size())
			fail("type mismatch: if without else has a different result arity");
		for (std::size_t i = 0; i < current.endTypes.size(); ++i) {
			if (!assignable(current.startTypes[i], current.endTypes[i]))
				fail("type mismatch: if without else has a different result type");
		}
	}
	const std::vector<ValueType> results = current.endTypes;
	const std::size_t height = current.height;
	controlStack.pop_back();
	typeStack.resize(height);
	pushTypes(results);
}

void Validator::branch(LabelIdx label)
{
	if (label >= controlStack.size())
		fail("unknown label " + std::to_string(label));
	const ControlFrame& target = controlStack[controlStack.size() - 1 - label];
	popTypes(target.isLoop ? target.startTypes : target.endTypes);
	setUnreachable();
}

void Validator::branchIf(LabelIdx label)
{
	popExpect(ValueTypeCode::I32);
	if (label >= controlStack.size())
		fail("unknown label " + std::to_string(label));
	const ControlFrame& target = controlStack[controlStack.size() - 1 - label];
	const std::vector<ValueType>& types = target.isLoop ? target.startTypes : target.endTypes;
	popTypes(types);
	pushTypes(types);
}

ValueTypeCode Validator::memoryAddressType(MemIdx memIdx) const
{
	return memoryIs64(memIdx) ? ValueTypeCode::I64 : ValueTypeCode::I32;
}

ValueTypeCode Validator::tableIndexType(TableIdx tableIdx) const
{
	const Limits& limits = tableIdx < module.importTables.size()
			? module.importTables[tableIdx].table.limits
			: module.tables[tableIdx - module.importTables.size()].limits;
	return (limits.flags & 0x04) != 0 ? ValueTypeCode::I64 : ValueTypeCode::I32;
}

FuncType Validator::functionType(FuncIdx idx) const
{
	requireFunctionIndex(idx);
	if (idx < module.importFunctions.size())
		return funcTypeAt(module.importFunctions[idx].typeIdx);
	return funcTypeAt(module.internalFunctionTypeIndices[idx - module.importFunctions.size()]);
}

ValueType Validator::globalType(GlobalIdx idx) const
{
	requireGlobalIndex(idx);
	if (idx < module.importGlobals.size())
		return module.importGlobals[idx].global.contentType;
	return module.globals[idx - module.importGlobals.size()].type.contentType;
}

ValueType Validator::tableElementType(TableIdx idx) const
{
	requireTableIndex(idx);
	if (idx < module.importTables.size())
		return module.importTables[idx].table.elementType;
	return module.tables[idx - module.importTables.size()].elementType;
}

void Validator::validate()
{
	validateModule();
	for (std::size_t i = 0; i < module.functionBodies.size(); ++i)
		validateFunctionBody(module.functionBodies[i], i);
}

void Validator::onUnhandledOpcode(const char*)
{
	// No typing rule for this opcode (SIMD/GC/atomic/exception forms in this
	// milestone). Keep scanning for immediate checks, but stop trusting the
	// operand stack for the rest of the body.
	typeTainted = true;
}

void Validator::validateModule()
{
	// A function/tag type index must name a function type.
	for (const ImportFunction& imp : module.importFunctions)
		funcTypeAt(imp.typeIdx);
	for (uint32_t typeIdx : module.internalFunctionTypeIndices)
		funcTypeAt(typeIdx);
	for (std::size_t i = 0; i < module.importTags.size(); ++i) {
		if (!funcTypeAt(module.importTags[i].typeIdx).results.empty())
			fail("tag " + module.describeIndex(NameKind::Tag, static_cast<uint32_t>(i))
				 + " has a non-empty result type");
	}
	for (std::size_t i = 0; i < module.tags.size(); ++i) {
		if (!funcTypeAt(module.tags[i].typeIdx).results.empty())
			fail("tag " + module.describeIndex(NameKind::Tag,
					 static_cast<uint32_t>(module.importTags.size() + i))
				 + " has a non-empty result type");
	}

	// Every typed reference must name a declared type. This walks the canonical
	// view directly: no module-owned Subtype and no module-local index is read.
	const auto checkStorage = [this](const CanonStorageType& storage, const char* where) {
		if (storage.isPacked)
			return;
		const CanonValueType& value = storage.value;
		if ((value.opcode == ValueTypeCode::Ref || value.opcode == ValueTypeCode::RefNull)
				&& value.heap.kind == CanonHeapType::Kind::Global
				&& !Store::global().types().isCoreType(value.heap.id))
			fail(std::string(where) + ": unknown type");
	};
	for (std::size_t i = 0; i < module.typeCount(); ++i) {
		const CoreTypeView view = module.typeView(static_cast<uint32_t>(i));
		switch (view.kind()) {
		case CanonTypeKind::Func:
			for (const CanonStorageType& p : view.funcParams())
				checkStorage(p, "function type parameter");
			for (const CanonStorageType& r : view.funcResults())
				checkStorage(r, "function type result");
			break;
		case CanonTypeKind::Struct:
			for (const CanonFieldType& field : view.structFields())
				checkStorage(field.storage, "struct field");
			break;
		case CanonTypeKind::Array:
			checkStorage(view.arrayElement().storage, "array element");
			break;
		case CanonTypeKind::Import:
			// An imported type is bounded by an abstract heap type only.
			break;
		}
	}
	for (const ImportTable& imp : module.importTables)
		requireWellFormedType(imp.table.elementType, "imported table");
	for (const TableType& table : module.tables)
		requireWellFormedType(table.elementType, "table");
	for (const ImportGlobal& imp : module.importGlobals)
		requireWellFormedType(imp.global.contentType, "imported global");
	for (const Global& global : module.globals)
		requireWellFormedType(global.type.contentType, "global");
	for (const ElementSegment& segment : module.elementSegments)
		requireWellFormedType(segment.elemType, "element segment");
	for (const FunctionBody& body : module.functionBodies)
		for (const LocalEntry& local : body.locals)
			requireWellFormedType(local.type, "local");

	if (module.functionBodies.size() != module.internalFunctionTypeIndices.size())
		fail("function and code section have inconsistent lengths");
	if (module.hasDataCount && module.dataSegmentCount != module.dataSegments.size())
		fail("data count and data section have inconsistent lengths");

	if (module.hasStartFunction) {
		requireFunctionIndex(module.startFunctionIndex);
		const FuncType& startType = module.startFunctionIndex < module.importFunctions.size()
				? funcTypeAt(module.importFunctions[module.startFunctionIndex].typeIdx)
				: funcTypeAt(module.internalFunctionTypeIndices[
						module.startFunctionIndex - module.importFunctions.size()]);
		if (!startType.params.empty() || !startType.results.empty())
			fail("start function "
				 + module.describeIndex(NameKind::Function, module.startFunctionIndex)
				 + " must have type [] -> []");
	}

	std::unordered_set<std::string> exportNames;
	for (const Export& ex : module.exports) {
		if (!exportNames.insert(ex.name).second)
			fail("duplicate export name '" + ex.name + "'");
		switch (ex.kind) {
		case ExternalKind::Function: requireFunctionIndex(ex.index); declaredFunctions.insert(ex.index); break;
		case ExternalKind::Table:    requireTableIndex(ex.index);    break;
		case ExternalKind::Memory:   requireMemoryIndex(ex.index);   break;
		case ExternalKind::Global:   requireGlobalIndex(ex.index);   break;
		case ExternalKind::Tag:      requireTagIndex(ex.index);      break;
		case ExternalKind::Type:
			if (ex.index >= module.typeCount())
				fail("unknown type " + module.describeIndex(NameKind::Type, ex.index));
			break;
		}
	}

	for (std::size_t i = 0; i < module.importMemories.size(); ++i) {
		const MemoryType& mt = module.importMemories[i].memory;
		checkLimits(mt.limits, mt.indexType == MemoryIndexType::I64, true, "memory", i);
	}
	for (std::size_t i = 0; i < module.memories.size(); ++i) {
		const MemoryType& mt = module.memories[i];
		checkLimits(mt.limits, mt.indexType == MemoryIndexType::I64, true, "memory", i);
	}
	for (std::size_t i = 0; i < module.importTables.size(); ++i) {
		const TableType& tt = module.importTables[i].table;
		checkLimits(tt.limits, (tt.limits.flags & 0x04) != 0, false, "table", i);
		validateTableInit(tt, i, true);
	}
	for (std::size_t i = 0; i < module.tables.size(); ++i) {
		const TableType& tt = module.tables[i];
		checkLimits(tt.limits, (tt.limits.flags & 0x04) != 0, false, "table", i);
		validateTableInit(tt, i);
	}

	for (std::size_t i = 0; i < module.globals.size(); ++i)
		validateGlobalInit(module.globals[i], i);
	for (std::size_t i = 0; i < module.elementSegments.size(); ++i)
		validateElementSegment(module.elementSegments[i], i);
	for (std::size_t i = 0; i < module.dataSegments.size(); ++i)
		validateDataSegment(module.dataSegments[i], i);
}

void Validator::validateGlobalInit(const Global& global, std::size_t index)
{
	const std::string label = "global " + std::to_string(index);
	if (global.initOpcode.empty())
		fail(label + ": missing initializer");

	const std::vector<uint8_t>& bytes = global.initOpcode;
	std::span<const std::byte> span(reinterpret_cast<const std::byte*>(bytes.data()), bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	FastMemReader stream(device);

	std::vector<ValueType> stack;
	const auto push = [&stack](ValueTypeCode code) {
		stack.push_back(ValueType{code, -1});
	};
	const auto pop = [&](ValueTypeCode expected) {
		if (stack.empty() || stack.back().opcode != expected)
			return false;
		stack.pop_back();
		return true;
	};

	bool done = false;
	while (!done) {
		uint8_t op = 0;
		stream >> op;
		switch (op) {
		case 0x41: { int32_t v; stream >> Elv::Io::Leb(v); push(ValueTypeCode::I32); break; }
		case 0x42: { int64_t v; stream >> Elv::Io::Leb(v); push(ValueTypeCode::I64); break; }
		case 0x43: device.seek(4, Elv::Io::SeekOrigin::CUR); push(ValueTypeCode::F32); break;
		case 0x44: device.seek(8, Elv::Io::SeekOrigin::CUR); push(ValueTypeCode::F64); break;
		case 0xD0: { // ref.null
			HeapType ht;
			stream >> ht;
			if (ht.isTypeIndex && ht.typeIndex >= module.typeCount())
				fail(label + ": unknown type");
			ValueType vt{};
			vt.opcode = ValueTypeCode::RefNull;
			vt.heapType = ht.isTypeIndex
					? static_cast<int32_t>(ht.typeIndex)
					: static_cast<int32_t>(ht.abstract);
			stack.push_back(vt);
			break;
		}
		case 0xD2: { // ref.func
			uint32_t funcIdx = 0;
			stream >> Elv::Io::Leb(funcIdx);
			requireFunctionIndex(funcIdx);
			declaredFunctions.insert(funcIdx);
			const uint32_t typeIdx = funcIdx < module.importFunctions.size()
					? module.importFunctions[funcIdx].typeIdx
					: module.internalFunctionTypeIndices[funcIdx - module.importFunctions.size()];
			// A function reference is non-null and has the function's concrete type.
			stack.push_back(ValueType{ValueTypeCode::Ref, static_cast<int32_t>(typeIdx)});
			break;
		}
		case 0x23: { // global.get: an earlier immutable global
			uint32_t globalIdx = 0;
			stream >> Elv::Io::Leb(globalIdx);
			const std::size_t imported = module.importGlobals.size();
			// The current global's absolute index is imported + index; a
			// constant expression may only read globals already declared.
			if (globalIdx >= imported + index)
				fail(label + ": initializer refers to a later global");
			const bool targetMutable = globalIdx < imported
					? module.importGlobals[globalIdx].global.isMutable
					: module.globals[globalIdx - imported].type.isMutable;
			if (targetMutable)
				fail(label + ": initializer may only read an immutable global");
			stack.push_back(globalIdx < imported
					? module.importGlobals[globalIdx].global.contentType
					: module.globals[globalIdx - imported].type.contentType);
			break;
		}
		case 0x6A: case 0x6B: case 0x6C: // i32.add/sub/mul (extended const)
			if (!pop(ValueTypeCode::I32) || !pop(ValueTypeCode::I32))
				fail(label + ": type mismatch in initializer");
			push(ValueTypeCode::I32);
			break;
		case 0x7C: case 0x7D: case 0x7E: // i64.add/sub/mul (extended const)
			if (!pop(ValueTypeCode::I64) || !pop(ValueTypeCode::I64))
				fail(label + ": type mismatch in initializer");
			push(ValueTypeCode::I64);
			break;
		case 0xFD: { // SIMD prefix: v128.const is the only const form here
			uint32_t subOpcode = 0;
			stream >> Elv::Io::Leb(subOpcode);
			if (subOpcode != 12)
				fail(label + ": constant expression required");
			device.seek(16, Elv::Io::SeekOrigin::CUR);
			push(ValueTypeCode::V128);
			break;
		}
		case 0xFB: { // GC prefix: the constructors/conversions are const-eligible
			uint32_t subOpcode = 0;
			stream >> Elv::Io::Leb(subOpcode);
			switch (subOpcode) {
			case 0x82: { // string.const idx (docs/proposals/STRINGREF.md STR-2)
				uint32_t idx = 0;
				stream >> Elv::Io::Leb(idx);
				if (idx >= module.stringLiterals.size())
					fail(label + ": string.const index out of range");
				push(ValueTypeCode::StringRef);
				break;
			}
			case 0: case 1: case 6: case 7: case 8: case 9: case 10:
			case 26: case 27: case 28:
				return; // modelled by evalConstantExpr; conservatively accepted
			default:
				fail(label + ": constant expression required");
			}
			// `string.const` is followed by the rest of the expression (at
			// least `end`); keep scanning so it is not silently truncated.
			break;
		}
		case 0x0B: // end
			done = true;
			break;
		default:
			fail(label + ": constant expression required");
		}
	}

	if (stack.size() != 1)
		fail(label + ": type mismatch in initializer");
	const ValueType& got = stack.back();
	const ValueType& want = global.type.contentType;
	if (!assignable(got, want))
		fail(label + ": initializer type does not match the global type");
}

std::optional<ValueType> Validator::validateConstExpr(const std::vector<uint8_t>& bytes,
		const std::string& label, std::size_t globalLimit)
{
	if (bytes.empty())
		fail(label + ": missing constant expression");
	std::span<const std::byte> span(reinterpret_cast<const std::byte*>(bytes.data()),
			bytes.size());
	Euph::Io::ConstBufferDevice device(span);
	FastMemReader stream(device);

	std::vector<ValueType> stack;
	const auto push = [&stack](const ValueType& type) { stack.push_back(type); };
	const auto pop = [&stack](ValueTypeCode expected) {
		if (stack.empty() || stack.back().opcode != expected)
			return false;
		stack.pop_back();
		return true;
	};

	bool done = false;
	while (!done) {
		uint8_t op = 0;
		stream >> op;
		switch (op) {
		case 0x41: { int32_t v; stream >> Elv::Io::Leb(v); push(ValueType{ValueTypeCode::I32, -1}); break; }
		case 0x42: { int64_t v; stream >> Elv::Io::Leb(v); push(ValueType{ValueTypeCode::I64, -1}); break; }
		case 0x43: device.seek(4, Elv::Io::SeekOrigin::CUR); push(ValueType{ValueTypeCode::F32, -1}); break;
		case 0x44: device.seek(8, Elv::Io::SeekOrigin::CUR); push(ValueType{ValueTypeCode::F64, -1}); break;
		case 0xD0: { // ref.null
			HeapType ht;
			stream >> ht;
			if (ht.isTypeIndex && ht.typeIndex >= module.typeCount())
				fail(label + ": unknown type");
			ValueType type{};
			type.opcode = ValueTypeCode::RefNull;
			type.heapType = ht.isTypeIndex
					? static_cast<int32_t>(ht.typeIndex)
					: static_cast<int32_t>(ht.abstract);
			push(type);
			break;
		}
		case 0xD2: { // ref.func
			uint32_t funcIdx = 0;
			stream >> Elv::Io::Leb(funcIdx);
			requireFunctionIndex(funcIdx);
			declaredFunctions.insert(funcIdx);
			const uint32_t typeIdx = funcIdx < module.importFunctions.size()
					? module.importFunctions[funcIdx].typeIdx
					: module.internalFunctionTypeIndices[funcIdx - module.importFunctions.size()];
			push(ValueType{ValueTypeCode::Ref, static_cast<int32_t>(typeIdx)});
			break;
		}
		case 0x23: { // global.get: immutable and already declared
			uint32_t globalIdx = 0;
			stream >> Elv::Io::Leb(globalIdx);
			if (globalIdx >= globalLimit)
				fail(label + ": unknown global "
					 + module.describeIndex(NameKind::Global, globalIdx));
			const std::size_t imported = module.importGlobals.size();
			const bool mutableGlobal = globalIdx < imported
					? module.importGlobals[globalIdx].global.isMutable
					: module.globals[globalIdx - imported].type.isMutable;
			if (mutableGlobal)
				fail(label + ": constant expression required");
			push(globalIdx < imported
					? module.importGlobals[globalIdx].global.contentType
					: module.globals[globalIdx - imported].type.contentType);
			break;
		}
		case 0x6A: case 0x6B: case 0x6C: // i32.add/sub/mul (extended const)
			if (!pop(ValueTypeCode::I32) || !pop(ValueTypeCode::I32))
				fail(label + ": type mismatch in constant expression");
			push(ValueType{ValueTypeCode::I32, -1});
			break;
		case 0x7C: case 0x7D: case 0x7E: // i64.add/sub/mul (extended const)
			if (!pop(ValueTypeCode::I64) || !pop(ValueTypeCode::I64))
				fail(label + ": type mismatch in constant expression");
			push(ValueType{ValueTypeCode::I64, -1});
			break;
		case 0xFD: { // v128.const
			uint32_t subOpcode = 0;
			stream >> Elv::Io::Leb(subOpcode);
			if (subOpcode != 12)
				fail(label + ": constant expression required");
			device.seek(16, Elv::Io::SeekOrigin::CUR);
			push(ValueType{ValueTypeCode::V128, -1});
			break;
		}
		case 0xFB: { // GC constructors: modelled by evalConstantExpr, accepted
			uint32_t subOpcode = 0;
			stream >> Elv::Io::Leb(subOpcode);
			switch (subOpcode) {
			case 0x82: { // string.const idx (docs/proposals/STRINGREF.md STR-2)
				uint32_t idx = 0;
				stream >> Elv::Io::Leb(idx);
				if (idx >= module.stringLiterals.size())
					fail(label + ": string.const index out of range");
				push(ValueType{ValueTypeCode::StringRef, -1});
				break;
			}
			case 0: case 1: case 6: case 7: case 8: case 9: case 10:
			case 26: case 27: case 28:
				return std::nullopt;
			default:
				fail(label + ": constant expression required");
			}
			break;
		}
		case 0x0B: // end
			done = true;
			break;
		default:
			fail(label + ": constant expression required");
		}
	}

	if (stack.size() != 1)
		fail(label + ": type mismatch");
	return stack.back();
}

void Validator::validateTableInit(const TableType& table, std::size_t index, bool isImport)
{
	const std::string label = "table " + std::to_string(index);
	if (table.initExpr.empty()) {
		// An imported table is supplied fully initialized by its provider; a
		// locally-defined table with a non-defaultable element type needs an
		// explicit initializer for every slot.
		if (!isImport && !isDefaultable(table.elementType))
			fail(label + ": non-defaultable element type needs an initializer");
		return;
	}
	const auto actual = validateConstExpr(table.initExpr, label + " initializer",
			module.importGlobals.size());
	if (actual && !assignable(*actual, table.elementType))
		fail(label + ": initializer type does not match the table type");
}

void Validator::validateElementSegment(const ElementSegment& segment, std::size_t index)
{
	const std::string label = "element segment " + std::to_string(index);
	requireWellFormedType(segment.elemType, label + " element type");
	const bool indexForm = !segment.initIndices.empty();
	std::optional<ValueType> activeTableType;
	if (segment.isActive()) {
		requireTableIndex(segment.tableIdx);
		activeTableType = tableElementType(segment.tableIdx);
		// An index-form segment stores non-null function references, so it may
		// fill a non-null table even though its static kind is `funcref`; each
		// entry is checked below. An expression-form segment has a declared type
		// that must itself fit the table.
		if (!indexForm && !assignable(segment.elemType, *activeTableType))
			fail(label + ": element type does not match the table type");
		if (segment.offsetExpr.empty())
			fail(label + ": missing offset");
		const auto offset = validateConstExpr(segment.offsetExpr, label + " offset", totalGlobals);
		if (offset && offset->opcode != tableIndexType(segment.tableIdx))
			fail(label + ": offset type mismatch");
	}
	for (uint32_t funcIdx : segment.initIndices) {
		requireFunctionIndex(funcIdx);
		declaredFunctions.insert(funcIdx);
		const uint32_t typeIdx = funcIdx < module.importFunctions.size()
				? module.importFunctions[funcIdx].typeIdx
				: module.internalFunctionTypeIndices[funcIdx - module.importFunctions.size()];
		const ValueType actual{ValueTypeCode::Ref, static_cast<int32_t>(typeIdx)};
		if (!assignable(actual, segment.elemType))
			fail(label + ": function element does not match the element type");
		if (activeTableType && !assignable(actual, *activeTableType))
			fail(label + ": function element does not match the table type");
	}
	for (const std::vector<uint8_t>& expr : segment.initExprs) {
		const auto actual = validateConstExpr(expr, label + " initializer", totalGlobals);
		if (actual && !assignable(*actual, segment.elemType))
			fail(label + ": initializer does not match the element type");
	}
}

void Validator::validateDataSegment(const DataSegment& segment, std::size_t index)
{
	// Mode bit 0: 0 = active, 1 = passive.
	if ((segment.mode & 0x01) != 0)
		return;
	const std::string label = "data segment " + std::to_string(index);
	requireMemoryIndex(segment.memoryIdx);
	if (segment.offsetExpr.empty())
		fail(label + ": missing offset");
	const auto offset = validateConstExpr(segment.offsetExpr, label + " offset", totalGlobals);
	const ValueTypeCode want = memoryIs64(segment.memoryIdx)
			? ValueTypeCode::I64 : ValueTypeCode::I32;
	if (offset && offset->opcode != want)
		fail(label + ": offset type mismatch");
}

void Validator::validateFunctionBody(const FunctionBody& body, std::size_t index)
{
	currentBody = index;
	const FuncType& funcType = funcTypeAt(module.internalFunctionTypeIndices[index]);
	currentFuncType = &funcType;
	locals.clear();
	for (const StorageType& param : funcType.params)
		locals.push_back(param.val);
	for (const LocalEntry& entry : body.locals) {
		if (entry.count > 100000 || locals.size() + entry.count > 100000)
			fail("too many locals");
		locals.insert(locals.end(), entry.count, entry.type);
	}

	typeStack.clear();
	controlStack.clear();
	typeTainted = false;
	controlStack.push_back(ControlFrame{{}, resultTypes(funcType), 0, false, false, false, false, {}, {}});
	std::vector<uint8_t> initialized(locals.size(), 0);
	for (std::size_t i = 0; i < locals.size(); ++i) {
		if (i < funcType.params.size() || isDefaultable(locals[i]))
			initialized[i] = 1;
	}
	controlStack.back().initialized = initialized;
	controlStack.back().entryInitialized = initialized;

	try {
		std::span<const std::byte> span(
				reinterpret_cast<const std::byte*>(body.code.data()), body.code.size());
		Euph::Io::ConstBufferDevice device(span);
		FastMemReader stream(device);
		readCode(stream);
	} catch (const ValidationError&) {
		throw;
	} catch (const std::exception& error) {
		fail(error.what());
	}
	currentBody = kNoBody;
}

void Validator::dispatchLocalGet(LocalIdx arg)
{
	requireLocalIndex(arg);
	if (!isDefaultable(locals[arg])) {
		const ControlFrame& current = controlStack.back();
		if (arg >= current.initialized.size() || !current.initialized[arg])
			fail("uninitialized local " + describeLocal(arg));
	}
	pushValue(locals[arg]);
}

void Validator::dispatchLocalSet(LocalIdx arg)
{
	requireLocalIndex(arg);
	popExpect(locals[arg]);
	controlStack.back().initialized[arg] = 1;
}

void Validator::dispatchLocalTee(LocalIdx arg)
{
	requireLocalIndex(arg);
	popExpect(locals[arg]);
	controlStack.back().initialized[arg] = 1;
	pushValue(locals[arg]);
}

void Validator::dispatchGlobalGet(GlobalIdx arg)
{
	requireGlobalIndex(arg);
	pushValue(globalType(arg));
}

void Validator::dispatchGlobalSet(GlobalIdx arg)
{
	requireGlobalIndex(arg);
	if (!globalIsMutable(arg))
		fail("global.set of an immutable global "
			 + module.describeIndex(NameKind::Global, arg));
	popExpect(globalType(arg));
}

void Validator::dispatchCall(FuncIdx arg)
{
	const FuncType& type = functionType(arg);
	popTypes(paramTypes(type));
	pushTypes(resultTypes(type));
}

void Validator::dispatchReturnCall(FuncIdx arg)
{
	const FuncType& type = functionType(arg);
	popTypes(paramTypes(type));
	checkTailCallResults(type);
	setUnreachable();
}

void Validator::dispatchCallIndirect(TypeIdx typeIdx, TableIdx tableIdx)
{
	const FuncType& type = funcTypeAt(typeIdx);
	requireTableIndex(tableIdx);
	if (!tableIsFuncRef(tableIdx))
		fail("call_indirect on a non-funcref table "
			 + module.describeIndex(NameKind::Table, tableIdx));
	popExpect(tableIndexType(tableIdx));
	popTypes(paramTypes(type));
	pushTypes(resultTypes(type));
}

void Validator::dispatchReturnCallIndirect(TypeIdx typeIdx, TableIdx tableIdx)
{
	const FuncType& type = funcTypeAt(typeIdx);
	requireTableIndex(tableIdx);
	if (!tableIsFuncRef(tableIdx))
		fail("return_call_indirect on a non-funcref table "
			 + module.describeIndex(NameKind::Table, tableIdx));
	popExpect(tableIndexType(tableIdx));
	popTypes(paramTypes(type));
	checkTailCallResults(type);
	setUnreachable();
}

void Validator::dispatchCallRef(TypeIdx typeIdx)
{
	const FuncType& type = funcTypeAt(typeIdx);
	popExpect(ValueType{ValueTypeCode::RefNull, static_cast<int32_t>(typeIdx)});
	popTypes(paramTypes(type));
	pushTypes(resultTypes(type));
}

void Validator::dispatchReturnCallRef(TypeIdx typeIdx)
{
	const FuncType& type = funcTypeAt(typeIdx);
	popExpect(ValueType{ValueTypeCode::RefNull, static_cast<int32_t>(typeIdx)});
	popTypes(paramTypes(type));
	checkTailCallResults(type);
	setUnreachable();
}

void Validator::dispatchRefFunc(FuncIdx arg)
{
	requireFunctionIndex(arg);
	if (declaredFunctions.find(arg) == declaredFunctions.end())
		fail("undeclared function reference "
			 + module.describeIndex(NameKind::Function, arg));
	const uint32_t typeIdx = arg < module.importFunctions.size()
			? module.importFunctions[arg].typeIdx
			: module.internalFunctionTypeIndices[arg - module.importFunctions.size()];
	// ref.func produces a *non-null* reference of the function's concrete type.
	pushValue(ValueType{ValueTypeCode::Ref, static_cast<int32_t>(typeIdx)});
}

void Validator::dispatchRefNull(const HeapType& heapType)
{
	if (heapType.isTypeIndex)
		(void)module.typeView(heapType.typeIndex);
	ValueType type{};
	type.opcode = ValueTypeCode::RefNull;
	type.heapType = heapType.isTypeIndex
			? static_cast<int32_t>(heapType.typeIndex)
			: static_cast<int32_t>(heapType.abstract);
	pushValue(type);
}

void Validator::dispatchRefTest(const HeapType& arg) { validateRefTest(false, arg); }
void Validator::dispatchRefTestNull(const HeapType& arg) { validateRefTest(true, arg); }
void Validator::dispatchRefCast(const HeapType& arg) { validateRefCast(false, arg); }
void Validator::dispatchRefCastNull(const HeapType& arg) { validateRefCast(true, arg); }

void Validator::dispatchThrow(TagIdx arg)
{
	requireTagIndex(arg);
	const uint32_t typeIdx = arg < module.importTags.size()
			? module.importTags[arg].typeIdx
			: module.tags[arg - module.importTags.size()].typeIdx;
	popTypes(paramTypes(funcTypeAt(typeIdx)));
	setUnreachable();
}

void Validator::dispatchThrowRef()
{
	popExpect(ValueTypeCode::ExnRef);
	setUnreachable();
}

void Validator::dispatchBrOnNull(LabelIdx label)
{
	const auto ref = popAny();
	if (!ref)
		return; // unreachable
	if (!refSubtype(*ref, *ref))
		fail("br_on_null requires a reference");
	if (label >= controlStack.size())
		fail("unknown label " + std::to_string(label));
	const ControlFrame& target = controlStack[controlStack.size() - 1 - label];
	const std::vector<ValueType>& types = target.isLoop ? target.startTypes : target.endTypes;
	// The branch passes the values below the reference (the label's types);
	// the non-null case falls through with those values *and* the refined ref.
	popTypes(types);
	pushTypes(types);

	ValueType refined{};
	refined.opcode = ValueTypeCode::Ref;
	refined.heapType = (ref->opcode == ValueTypeCode::Ref || ref->opcode == ValueTypeCode::RefNull)
			? ref->heapType
			: static_cast<int32_t>(ref->opcode);
	pushValue(refined);
}

void Validator::dispatchBrOnNonNull(LabelIdx label)
{
	const auto ref = popAny();
	if (!ref)
		return; // unreachable
	if (!refSubtype(*ref, *ref))
		fail("br_on_non_null requires a reference");
	if (label >= controlStack.size())
		fail("unknown label " + std::to_string(label));
	const ControlFrame& target = controlStack[controlStack.size() - 1 - label];
	const std::vector<ValueType>& types = target.isLoop ? target.startTypes : target.endTypes;
	if (types.empty())
		fail("type mismatch: br_on_non_null label arity");

	ValueType refined{};
	refined.opcode = ValueTypeCode::Ref;
	refined.heapType = (ref->opcode == ValueTypeCode::Ref || ref->opcode == ValueTypeCode::RefNull)
			? ref->heapType
			: static_cast<int32_t>(ref->opcode);
	if (!assignable(refined, types.back()))
		fail("type mismatch: br_on_non_null label type");

	// The branch passes the values below the ref plus the refined ref; the
	// null case falls through with only the values below it.
	std::vector<ValueType> prefix(types.begin(), types.end() - 1);
	popTypes(prefix);
	pushTypes(prefix);
}

ValueType Validator::refType(bool nullable, const HeapType& heapType) const
{
	ValueType type{};
	type.opcode = nullable ? ValueTypeCode::RefNull : ValueTypeCode::Ref;
	type.heapType = heapType.isTypeIndex
			? static_cast<int32_t>(heapType.typeIndex)
			: static_cast<int32_t>(heapType.abstract);
	return type;
}

ValueType Validator::refTopForHeap(const HeapType& heapType) const
{
	ValueType top{};
	top.heapType = -1;
	if (heapType.isTypeIndex) {
		const CoreTypeView view = module.typeView(heapType.typeIndex);
		if (view.kind() == CanonTypeKind::Import) {
			switch (view.importBound().abstract) {
			case AbstractHeapType::Func:
				top.opcode = ValueTypeCode::FuncRef;
				break;
			case AbstractHeapType::Extern:
				top.opcode = ValueTypeCode::ExternRef;
				break;
			default:
				top.opcode = ValueTypeCode::AnyRef;
				break;
			}
		} else {
			top.opcode = view.kind() == CanonTypeKind::Func
					? ValueTypeCode::FuncRef : ValueTypeCode::AnyRef;
		}
		return top;
	}
	switch (heapType.abstract) {
		case AbstractHeapType::Func:
		case AbstractHeapType::NoFunc:
			top.opcode = ValueTypeCode::FuncRef;
			break;
		case AbstractHeapType::Extern:
		case AbstractHeapType::NoExtern:
			top.opcode = ValueTypeCode::ExternRef;
			break;
		case AbstractHeapType::Exn:
		case AbstractHeapType::NoExn:
			top.opcode = ValueTypeCode::ExnRef;
			break;
		default:
			// Any/Eq/I31/Struct/Array/None share the `any` hierarchy.
			top.opcode = ValueTypeCode::AnyRef;
			break;
	}
	return top;
}

void Validator::validateRefTest(bool nullable, const HeapType& heapType)
{
	const ValueType target = refType(nullable, heapType);
	requireWellFormedType(target, "ref.test target");
	const ValueType top = refTopForHeap(heapType);
	const auto actual = popAny();
	if (!actual)
		return; // unreachable: the stack is polymorphic
	if (!assignable(*actual, top))
		fail("type mismatch: ref.test operand is outside the target hierarchy");
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::validateRefCast(bool nullable, const HeapType& heapType)
{
	const ValueType target = refType(nullable, heapType);
	requireWellFormedType(target, "ref.cast target");
	const ValueType top = refTopForHeap(heapType);
	const auto actual = popAny();
	if (!actual)
		return; // unreachable: the stack is polymorphic
	if (!assignable(*actual, top))
		fail("type mismatch: ref.cast operand is outside the target hierarchy");
	pushValue(target);
}

void Validator::dispatchBrOnCast(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2)
{
	validateBrOnCast(false, castop, label, ht1, ht2);
}

void Validator::dispatchBrOnCastFail(uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2)
{
	validateBrOnCast(true, castop, label, ht1, ht2);
}

void Validator::validateBrOnCast(bool isFail, uint8_t castop, LabelIdx label, HeapType ht1, HeapType ht2)
{
	const auto makeRef = [](bool nullable, const HeapType& ht) {
		ValueType type{};
		type.opcode = nullable ? ValueTypeCode::RefNull : ValueTypeCode::Ref;
		type.heapType = ht.isTypeIndex ? static_cast<int32_t>(ht.typeIndex)
									   : static_cast<int32_t>(ht.abstract);
		return type;
	};
	// castop bit 0 = the input is nullable; bit 1 = the cast result is nullable.
	const bool inputNullable = (castop & 0x01) != 0;
	const bool castNullable = (castop & 0x02) != 0;
	const ValueType input = makeRef(inputNullable, ht1);
	const ValueType cast = makeRef(castNullable, ht2);
	// The non-cast remainder `rt1 \ rt2`: when the cast target is nullable, null
	// is part of the cast, so the remainder is non-null.
	const ValueType difference = makeRef(inputNullable && !castNullable, ht1);
	requireWellFormedType(input, "br_on_cast input");
	requireWellFormedType(cast, "br_on_cast target");
	if (!assignable(cast, input))
		fail("type mismatch: br_on_cast target is not a subtype of the input");

	const auto actual = popAny();
	if (!actual)
		return; // unreachable
	if (!assignable(*actual, input))
		fail("type mismatch: br_on_cast input");
	if (label >= controlStack.size())
		fail("unknown label " + std::to_string(label));
	const ControlFrame& target = controlStack[controlStack.size() - 1 - label];
	const std::vector<ValueType>& labelTypes = target.isLoop ? target.startTypes : target.endTypes;
	if (labelTypes.empty())
		fail("type mismatch: br_on_cast label arity");

	// br_on_cast branches with the cast value and falls through with the
	// remainder; br_on_cast_fail is the other way round.
	const ValueType branchValue = isFail ? difference : cast;
	if (!assignable(branchValue, labelTypes.back()))
		fail("type mismatch: br_on_cast label type");
	std::vector<ValueType> prefix(labelTypes.begin(), labelTypes.end() - 1);
	popTypes(prefix);
	pushTypes(prefix);
	pushValue(isFail ? cast : difference);
}

void Validator::dispatchTryTable(const BlockType& blockType, std::vector<CatchClause>&& catches)
{
	for (const CatchClause& clause : catches) {
		if (clause.kind == CatchKind::Catch || clause.kind == CatchKind::CatchRef)
			requireTagIndex(clause.tagIdx);
		if (clause.labelIdx >= controlStack.size())
			fail("try_table catch: unknown label " + std::to_string(clause.labelIdx));

		// Catch labels are resolved in the *enclosing* context (the try_table's
		// own label is introduced inside its body and is not a catch target).
		const ControlFrame& target = controlStack[controlStack.size() - 1 - clause.labelIdx];
		const std::vector<ValueType>& labelTypes = target.isLoop ? target.startTypes : target.endTypes;
		std::vector<ValueType> payload;
		if (clause.kind == CatchKind::Catch || clause.kind == CatchKind::CatchRef) {
			const uint32_t typeIdx = clause.tagIdx < module.importTags.size()
					? module.importTags[clause.tagIdx].typeIdx
					: module.tags[clause.tagIdx - module.importTags.size()].typeIdx;
			payload = paramTypes(funcTypeAt(typeIdx));
			if (clause.kind == CatchKind::CatchRef)
				payload.push_back(ValueType{ValueTypeCode::Ref,
						static_cast<int32_t>(ValueTypeCode::ExnRef)});
		} else if (clause.kind == CatchKind::CatchAllRef) {
			payload.push_back(ValueType{ValueTypeCode::Ref,
					static_cast<int32_t>(ValueTypeCode::ExnRef)});
		}
		if (payload.size() != labelTypes.size())
			fail("type mismatch: catch label arity");
		for (std::size_t i = 0; i < payload.size(); ++i) {
			if (!assignable(payload[i], labelTypes[i]))
				fail("type mismatch: catch label type");
		}
	}
	beginBlock(blockType, false, false);
	// A catch injects its payload into an *outer* label, which this linear
	// stack does not model; keep immediate/index checks for the rest of the
	// body but stop trusting the operand stack when there are catches.
	if (!catches.empty())
		typeTainted = true;
}

void Validator::dispatchSelectT(std::vector<ValueType>&& types)
{
	if (types.size() != 1)
		fail("select: invalid result arity");
	requireWellFormedType(types[0], "select result");
	popExpect(ValueTypeCode::I32);
	popExpect(types[0]);
	popExpect(types[0]);
	pushValue(types[0]);
}

void Validator::dispatchTableGet(TableIdx arg)
{
	const ValueType element = tableElementType(arg);
	popExpect(tableIndexType(arg));
	pushValue(element);
}

void Validator::dispatchTableSet(TableIdx arg)
{
	const ValueType element = tableElementType(arg);
	popExpect(element);
	popExpect(tableIndexType(arg));
}

void Validator::dispatchTableGrow(TableIdx arg)
{
	const ValueType element = tableElementType(arg);
	const ValueTypeCode index = tableIndexType(arg);
	popExpect(index);
	popExpect(element);
	pushValue(ValueType{index, -1});
}

void Validator::dispatchTableSize(TableIdx arg)
{
	pushValue(ValueType{tableIndexType(arg), -1});
}

void Validator::dispatchTableFill(TableIdx arg)
{
	const ValueType element = tableElementType(arg);
	const ValueTypeCode index = tableIndexType(arg);
	popExpect(index);
	popExpect(element);
	popExpect(index);
}

void Validator::dispatchTableCopy(TableIdx dst, TableIdx src)
{
	popExpect(tableIndexType(dst)); // len
	popExpect(tableIndexType(src));
	popExpect(tableIndexType(dst));
}

void Validator::dispatchTableInit(uint32_t elemIdx, TableIdx tableIdx)
{
	requireElementIndex(elemIdx);
	const ElementSegment& segment = module.elementSegments[elemIdx];
	const ValueType tableType = tableElementType(tableIdx);
	if (segment.initIndices.empty()) {
		if (!assignable(segment.elemType, tableType))
			fail("table.init: element segment "
				 + module.describeIndex(NameKind::Element, elemIdx)
				 + " type does not match table "
				 + module.describeIndex(NameKind::Table, tableIdx) + " type");
	} else {
		// Index-form entries are non-null function references.
		for (uint32_t funcIdx : segment.initIndices) {
			requireFunctionIndex(funcIdx);
			const uint32_t typeIdx = funcIdx < module.importFunctions.size()
					? module.importFunctions[funcIdx].typeIdx
					: module.internalFunctionTypeIndices[funcIdx - module.importFunctions.size()];
			if (!assignable(ValueType{ValueTypeCode::Ref, static_cast<int32_t>(typeIdx)}, tableType))
				fail("table.init: function element "
					 + module.describeIndex(NameKind::Function, funcIdx)
					 + " does not match table "
					 + module.describeIndex(NameKind::Table, tableIdx) + " type");
		}
	}
	popExpect(ValueTypeCode::I32);          // len
	popExpect(ValueTypeCode::I32);          // src
	popExpect(tableIndexType(tableIdx));    // dst
}

void Validator::dispatchElemDrop(uint32_t elemIdx) { requireElementIndex(elemIdx); }

void Validator::dispatchMemorySize(MemIdx arg)
{
	requireMemoryIndex(arg);
	pushValue(ValueType{memoryAddressType(arg), -1});
}

void Validator::dispatchMemoryGrow(MemIdx arg)
{
	requireMemoryIndex(arg);
	const ValueTypeCode address = memoryAddressType(arg);
	popExpect(address);
	pushValue(ValueType{address, -1});
}

void Validator::dispatchMemoryInit(uint32_t dataIdx, MemIdx memIdx)
{
	if (!module.hasDataCount)
		fail("memory.init requires a data count section");
	requireMemoryIndex(memIdx);
	requireDataIndex(dataIdx);
	const ValueTypeCode address = memoryAddressType(memIdx);
	popExpect(ValueTypeCode::I32); // len
	popExpect(ValueTypeCode::I32); // src
	popExpect(address);            // dst
}

void Validator::dispatchDataDrop(uint32_t dataIdx)
{
	if (!module.hasDataCount)
		fail("data.drop requires a data count section");
	requireDataIndex(dataIdx);
}

void Validator::dispatchMemoryCopy(MemIdx dst, MemIdx src)
{
	requireMemoryIndex(dst);
	requireMemoryIndex(src);
	popExpect(memoryAddressType(dst)); // len (the destination's index type)
	popExpect(memoryAddressType(src));
	popExpect(memoryAddressType(dst));
}

void Validator::dispatchMemoryFill(MemIdx arg)
{
	requireMemoryIndex(arg);
	const ValueTypeCode address = memoryAddressType(arg);
	popExpect(address);            // len
	popExpect(ValueTypeCode::I32); // value
	popExpect(address);            // dst
}

void Validator::dispatchMemoryDiscard(MemIdx arg)
{
	requireMemoryIndex(arg);
	const ValueTypeCode address = memoryAddressType(arg);
	popExpect(address); // len
	popExpect(address); // dst
}

void Validator::dispatchI64Add128()
{
	popExpect(ValueTypeCode::I64); // rhs high
	popExpect(ValueTypeCode::I64); // rhs low
	popExpect(ValueTypeCode::I64); // lhs high
	popExpect(ValueTypeCode::I64); // lhs low
	pushValue(ValueType{ValueTypeCode::I64, -1}); // low
	pushValue(ValueType{ValueTypeCode::I64, -1}); // high
}

void Validator::dispatchI64Sub128()
{
	popExpect(ValueTypeCode::I64); // rhs high
	popExpect(ValueTypeCode::I64); // rhs low
	popExpect(ValueTypeCode::I64); // lhs high
	popExpect(ValueTypeCode::I64); // lhs low
	pushValue(ValueType{ValueTypeCode::I64, -1}); // low
	pushValue(ValueType{ValueTypeCode::I64, -1}); // high
}

void Validator::dispatchI64MulWideS()
{
	popExpect(ValueTypeCode::I64); // rhs
	popExpect(ValueTypeCode::I64); // lhs
	pushValue(ValueType{ValueTypeCode::I64, -1}); // low
	pushValue(ValueType{ValueTypeCode::I64, -1}); // high
}

void Validator::dispatchI64MulWideU()
{
	popExpect(ValueTypeCode::I64); // rhs
	popExpect(ValueTypeCode::I64); // lhs
	pushValue(ValueType{ValueTypeCode::I64, -1}); // low
	pushValue(ValueType{ValueTypeCode::I64, -1}); // high
}

#define WJ_LOAD(name, result, naturalLog2) \
	void Validator::dispatch##name(MemArg arg) { \
		requireMemoryAccess(arg, naturalLog2); \
		popExpect(memoryAddressType(arg.memidx)); \
		pushValue(ValueType{ValueTypeCode::result, -1}); \
	}
#define WJ_STORE(name, value, naturalLog2) \
	void Validator::dispatch##name(MemArg arg) { \
		requireMemoryAccess(arg, naturalLog2); \
		popExpect(ValueTypeCode::value); \
		popExpect(memoryAddressType(arg.memidx)); \
	}

WJ_LOAD(I32Load, I32, 2)
WJ_LOAD(I64Load, I64, 3)
WJ_LOAD(F32Load, F32, 2)
WJ_LOAD(F64Load, F64, 3)
WJ_LOAD(I32Load8S, I32, 0)
WJ_LOAD(I32Load8U, I32, 0)
WJ_LOAD(I32Load16S, I32, 1)
WJ_LOAD(I32Load16U, I32, 1)
WJ_LOAD(I64Load8S, I64, 0)
WJ_LOAD(I64Load8U, I64, 0)
WJ_LOAD(I64Load16S, I64, 1)
WJ_LOAD(I64Load16U, I64, 1)
WJ_LOAD(I64Load32S, I64, 2)
WJ_LOAD(I64Load32U, I64, 2)
WJ_STORE(I32Store, I32, 2)
WJ_STORE(I64Store, I64, 3)
WJ_STORE(F32Store, F32, 2)
WJ_STORE(F64Store, F64, 3)
WJ_STORE(I32Store8, I32, 0)
WJ_STORE(I32Store16, I32, 1)
WJ_STORE(I64Store8, I64, 0)
WJ_STORE(I64Store16, I64, 1)
WJ_STORE(I64Store32, I64, 2)

#undef WJ_LOAD
#undef WJ_STORE

#define WJ_SIMD_LOAD(name, naturalLog2) \
	void Validator::dispatch##name(MemArg arg) { \
		requireMemoryAccess(arg, naturalLog2); \
		popExpect(memoryAddressType(arg.memidx)); \
		pushValue(ValueType{ValueTypeCode::V128, -1}); \
	}
#define WJ_SIMD_STORE(name, naturalLog2) \
	void Validator::dispatch##name(MemArg arg) { \
		requireMemoryAccess(arg, naturalLog2); \
		popExpect(ValueTypeCode::V128); \
		popExpect(memoryAddressType(arg.memidx)); \
	}

WJ_SIMD_LOAD(V128Load, 4)
WJ_SIMD_LOAD(V128Load8x8S, 3)
WJ_SIMD_LOAD(V128Load8x8U, 3)
WJ_SIMD_LOAD(V128Load16x4S, 3)
WJ_SIMD_LOAD(V128Load16x4U, 3)
WJ_SIMD_LOAD(V128Load32x2S, 3)
WJ_SIMD_LOAD(V128Load32x2U, 3)
WJ_SIMD_LOAD(V128Load8Splat, 0)
WJ_SIMD_LOAD(V128Load16Splat, 1)
WJ_SIMD_LOAD(V128Load32Splat, 2)
WJ_SIMD_LOAD(V128Load64Splat, 3)
WJ_SIMD_LOAD(V128Load32Zero, 2)
WJ_SIMD_LOAD(V128Load64Zero, 3)
WJ_SIMD_STORE(V128Store, 4)

#undef WJ_SIMD_LOAD
#undef WJ_SIMD_STORE

#define WJ_SIMD_LANE_LOAD(name, count, naturalLog2, text) \
	void Validator::dispatch##name(MemArg arg, uint8_t lane) { \
		requireMemoryAccess(arg, naturalLog2); \
		requireLane(lane, count, text); \
		popExpect(ValueTypeCode::V128); \
		popExpect(memoryAddressType(arg.memidx)); \
		pushValue(ValueType{ValueTypeCode::V128, -1}); \
	}
#define WJ_SIMD_LANE_STORE(name, count, naturalLog2, text) \
	void Validator::dispatch##name(MemArg arg, uint8_t lane) { \
		requireMemoryAccess(arg, naturalLog2); \
		requireLane(lane, count, text); \
		popExpect(ValueTypeCode::V128); \
		popExpect(memoryAddressType(arg.memidx)); \
	}

WJ_SIMD_LANE_LOAD(V128Load8Lane, 16, 0, "v128.load8_lane")
WJ_SIMD_LANE_LOAD(V128Load16Lane, 8, 1, "v128.load16_lane")
WJ_SIMD_LANE_LOAD(V128Load32Lane, 4, 2, "v128.load32_lane")
WJ_SIMD_LANE_LOAD(V128Load64Lane, 2, 3, "v128.load64_lane")
WJ_SIMD_LANE_STORE(V128Store8Lane, 16, 0, "v128.store8_lane")
WJ_SIMD_LANE_STORE(V128Store16Lane, 8, 1, "v128.store16_lane")
WJ_SIMD_LANE_STORE(V128Store32Lane, 4, 2, "v128.store32_lane")
WJ_SIMD_LANE_STORE(V128Store64Lane, 2, 3, "v128.store64_lane")

#undef WJ_SIMD_LANE_LOAD
#undef WJ_SIMD_LANE_STORE

void Validator::dispatchI8x16Shuffle(std::span<uint8_t> lanes)
{
	for (uint8_t lane : lanes) {
		if (lane >= 32)
			fail("i8x16.shuffle: invalid lane index " + std::to_string(lane));
	}
	popExpect(ValueTypeCode::V128);
	popExpect(ValueTypeCode::V128);
	pushValue(ValueType{ValueTypeCode::V128, -1});
}

#define WJ_SIMD_EXTRACT(name, count, T, text) \
	void Validator::dispatch##name(uint8_t lane) { \
		requireLane(lane, count, text); \
		popExpect(ValueTypeCode::V128); \
		pushValue(ValueType{ValueTypeCode::T, -1}); \
	}
#define WJ_SIMD_REPLACE(name, count, T, text) \
	void Validator::dispatch##name(uint8_t lane) { \
		requireLane(lane, count, text); \
		popExpect(ValueTypeCode::T); \
		popExpect(ValueTypeCode::V128); \
		pushValue(ValueType{ValueTypeCode::V128, -1}); \
	}

WJ_SIMD_EXTRACT(I8x16ExtractLaneS, 16, I32, "i8x16.extract_lane")
WJ_SIMD_EXTRACT(I8x16ExtractLaneU, 16, I32, "i8x16.extract_lane")
WJ_SIMD_REPLACE(I8x16ReplaceLane, 16, I32, "i8x16.replace_lane")
WJ_SIMD_EXTRACT(I16x8ExtractLaneS, 8, I32, "i16x8.extract_lane")
WJ_SIMD_EXTRACT(I16x8ExtractLaneU, 8, I32, "i16x8.extract_lane")
WJ_SIMD_REPLACE(I16x8ReplaceLane, 8, I32, "i16x8.replace_lane")
WJ_SIMD_EXTRACT(I32x4ExtractLane, 4, I32, "i32x4.extract_lane")
WJ_SIMD_REPLACE(I32x4ReplaceLane, 4, I32, "i32x4.replace_lane")
WJ_SIMD_EXTRACT(I64x2ExtractLane, 2, I64, "i64x2.extract_lane")
WJ_SIMD_REPLACE(I64x2ReplaceLane, 2, I64, "i64x2.replace_lane")
WJ_SIMD_EXTRACT(F32x4ExtractLane, 4, F32, "f32x4.extract_lane")
WJ_SIMD_REPLACE(F32x4ReplaceLane, 4, F32, "f32x4.replace_lane")
WJ_SIMD_EXTRACT(F64x2ExtractLane, 2, F64, "f64x2.extract_lane")
WJ_SIMD_REPLACE(F64x2ReplaceLane, 2, F64, "f64x2.replace_lane")

#undef WJ_SIMD_EXTRACT
#undef WJ_SIMD_REPLACE


void Validator::dispatchUnreachable() { setUnreachable(); }
void Validator::dispatchNop() {}

void Validator::dispatchBlock(const BlockType& arg) { beginBlock(arg, false, false); }
void Validator::dispatchLoop(const BlockType& arg) { beginBlock(arg, true, false); }

void Validator::dispatchIf(const BlockType& arg)
{
	popExpect(ValueTypeCode::I32);
	beginBlock(arg, false, true);
}

void Validator::dispatchElse() { endBlock(true); }
void Validator::dispatchEnd() { endBlock(false); }

void Validator::dispatchBr(LabelIdx arg) { branch(arg); }
void Validator::dispatchBrIf(LabelIdx arg) { branchIf(arg); }

void Validator::dispatchBrTable(std::vector<LabelIdx>&& labels, LabelIdx defaultLabel)
{
	popExpect(ValueTypeCode::I32);
	if (defaultLabel >= controlStack.size())
		fail("unknown label " + std::to_string(defaultLabel));
	if (frame().unreachable || typeTainted) {
		// The operand stack is polymorphic after `unreachable`, so the target
		// result types need not agree (unreached-valid.wast's `meet-bottom`).
		// Label indices are still checked.
		for (LabelIdx label : labels) {
			if (label >= controlStack.size())
				fail("unknown label " + std::to_string(label));
		}
		setUnreachable();
		return;
	}
	const auto typesFor = [this](LabelIdx label) -> const std::vector<ValueType>& {
		const ControlFrame& target = controlStack[controlStack.size() - 1 - label];
		return target.isLoop ? target.startTypes : target.endTypes;
	};
	const std::vector<ValueType> defaultTypes = typesFor(defaultLabel);
	const std::size_t arity = defaultTypes.size();
	if (typeStack.size() < arity)
		fail("type mismatch: not enough operands for br_table");
	// The values on the stack must be assignable to *every* target type. The
	// default target alone is not enough: with reference subtyping, one target
	// can be more specific than the default (br_table.wast's `$l1`/`$l2`).
	const std::vector<ValueType> actual(typeStack.end() - static_cast<std::ptrdiff_t>(arity),
			typeStack.end());
	for (LabelIdx label : labels) {
		if (label >= controlStack.size())
			fail("unknown label " + std::to_string(label));
		const std::vector<ValueType>& types = typesFor(label);
		if (types.size() != arity)
			fail("type mismatch: br_table target arity differs");
		for (std::size_t i = 0; i < arity; ++i) {
			if (!assignable(actual[i], types[i]))
				fail("type mismatch: br_table target type differs");
		}
	}
	popTypes(defaultTypes);
	setUnreachable();
}

void Validator::dispatchReturn()
{
	if (currentFuncType != nullptr)
		popTypes(resultTypes(*currentFuncType));
	setUnreachable();
}

void Validator::dispatchDrop() { (void)popAny(); }

void Validator::dispatchSelect()
{
	popExpect(ValueTypeCode::I32);
	const auto type = popAny();
	if (!type)
		return;
	// The untyped form is only defined for numeric/vector operands; a reference
	// needs the explicit `(result t)` form.
	switch (type->opcode) {
	case ValueTypeCode::I32:
	case ValueTypeCode::I64:
	case ValueTypeCode::F32:
	case ValueTypeCode::F64:
	case ValueTypeCode::V128:
		break;
	default:
		fail("select: reference type requires an explicit result type");
	}
	popExpect(*type);
	pushValue(*type);
}

void Validator::dispatchI32Const(int32_t) { pushValue(ValueType{ValueTypeCode::I32, -1}); }
void Validator::dispatchI64Const(int64_t) { pushValue(ValueType{ValueTypeCode::I64, -1}); }
void Validator::dispatchF32Const(float) { pushValue(ValueType{ValueTypeCode::F32, -1}); }
void Validator::dispatchF64Const(double) { pushValue(ValueType{ValueTypeCode::F64, -1}); }

void Validator::dispatchRefIsNull()
{
	(void)popAny();
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchRefEq()
{
	const auto right = popAny();
	const auto left = popAny();
	if (right && !isEqRef(*right))
		fail("ref.eq requires eqref operands");
	if (left && !isEqRef(*left))
		fail("ref.eq requires eqref operands");
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchRefAsNonNull()
{
	const auto type = popAny();
	if (type)
		pushValue(*type);
}



// Atomic memory accesses: loads/stores/RMW/cmpxchg shapes.
#define WJ_ATOMIC_LOAD(n, T, natural) \
	void Validator::dispatch##n(MemArg a) { requireMemoryAccess(a, natural); popExpect(memoryAddressType(a.memidx)); pushValue(ValueType{ValueTypeCode::T, -1}); }
#define WJ_ATOMIC_STORE(n, T, natural) \
	void Validator::dispatch##n(MemArg a) { requireMemoryAccess(a, natural); popExpect(ValueTypeCode::T); popExpect(memoryAddressType(a.memidx)); }
#define WJ_ATOMIC_RMW(n, T, natural) \
	void Validator::dispatch##n(MemArg a) { requireMemoryAccess(a, natural); popExpect(ValueTypeCode::T); popExpect(memoryAddressType(a.memidx)); pushValue(ValueType{ValueTypeCode::T, -1}); }
#define WJ_ATOMIC_CMPXCHG(n, T, natural) \
	void Validator::dispatch##n(MemArg a) { requireMemoryAccess(a, natural); popExpect(ValueTypeCode::T); popExpect(ValueTypeCode::T); popExpect(memoryAddressType(a.memidx)); pushValue(ValueType{ValueTypeCode::T, -1}); }

WJ_ATOMIC_LOAD(I32AtomicLoad, I32, 2)
WJ_ATOMIC_LOAD(I32AtomicLoad16U, I32, 1)
WJ_ATOMIC_LOAD(I32AtomicLoad8U, I32, 0)
WJ_ATOMIC_RMW(I32AtomicRmw16AddU, I32, 1)
WJ_ATOMIC_RMW(I32AtomicRmw16AndU, I32, 1)
WJ_ATOMIC_CMPXCHG(I32AtomicRmw16CmpxchgU, I32, 1)
WJ_ATOMIC_RMW(I32AtomicRmw16OrU, I32, 1)
WJ_ATOMIC_RMW(I32AtomicRmw16SubU, I32, 1)
WJ_ATOMIC_RMW(I32AtomicRmw16XchgU, I32, 1)
WJ_ATOMIC_RMW(I32AtomicRmw16XorU, I32, 1)
WJ_ATOMIC_RMW(I32AtomicRmw8AddU, I32, 0)
WJ_ATOMIC_RMW(I32AtomicRmw8AndU, I32, 0)
WJ_ATOMIC_CMPXCHG(I32AtomicRmw8CmpxchgU, I32, 0)
WJ_ATOMIC_RMW(I32AtomicRmw8OrU, I32, 0)
WJ_ATOMIC_RMW(I32AtomicRmw8SubU, I32, 0)
WJ_ATOMIC_RMW(I32AtomicRmw8XchgU, I32, 0)
WJ_ATOMIC_RMW(I32AtomicRmw8XorU, I32, 0)
WJ_ATOMIC_RMW(I32AtomicRmwAdd, I32, 2)
WJ_ATOMIC_RMW(I32AtomicRmwAnd, I32, 2)
WJ_ATOMIC_CMPXCHG(I32AtomicRmwCmpxchg, I32, 2)
WJ_ATOMIC_RMW(I32AtomicRmwOr, I32, 2)
WJ_ATOMIC_RMW(I32AtomicRmwSub, I32, 2)
WJ_ATOMIC_RMW(I32AtomicRmwXchg, I32, 2)
WJ_ATOMIC_RMW(I32AtomicRmwXor, I32, 2)
WJ_ATOMIC_STORE(I32AtomicStore, I32, 2)
WJ_ATOMIC_STORE(I32AtomicStore16, I32, 1)
WJ_ATOMIC_STORE(I32AtomicStore8, I32, 0)
WJ_ATOMIC_LOAD(I64AtomicLoad, I64, 3)
WJ_ATOMIC_LOAD(I64AtomicLoad16U, I64, 1)
WJ_ATOMIC_LOAD(I64AtomicLoad32U, I64, 2)
WJ_ATOMIC_LOAD(I64AtomicLoad8U, I64, 0)
WJ_ATOMIC_RMW(I64AtomicRmw16AddU, I64, 1)
WJ_ATOMIC_RMW(I64AtomicRmw16AndU, I64, 1)
WJ_ATOMIC_CMPXCHG(I64AtomicRmw16CmpxchgU, I64, 1)
WJ_ATOMIC_RMW(I64AtomicRmw16OrU, I64, 1)
WJ_ATOMIC_RMW(I64AtomicRmw16SubU, I64, 1)
WJ_ATOMIC_RMW(I64AtomicRmw16XchgU, I64, 1)
WJ_ATOMIC_RMW(I64AtomicRmw16XorU, I64, 1)
WJ_ATOMIC_RMW(I64AtomicRmw32AddU, I64, 2)
WJ_ATOMIC_RMW(I64AtomicRmw32AndU, I64, 2)
WJ_ATOMIC_CMPXCHG(I64AtomicRmw32CmpxchgU, I64, 2)
WJ_ATOMIC_RMW(I64AtomicRmw32OrU, I64, 2)
WJ_ATOMIC_RMW(I64AtomicRmw32SubU, I64, 2)
WJ_ATOMIC_RMW(I64AtomicRmw32XchgU, I64, 2)
WJ_ATOMIC_RMW(I64AtomicRmw32XorU, I64, 2)
WJ_ATOMIC_RMW(I64AtomicRmw8AddU, I64, 0)
WJ_ATOMIC_RMW(I64AtomicRmw8AndU, I64, 0)
WJ_ATOMIC_CMPXCHG(I64AtomicRmw8CmpxchgU, I64, 0)
WJ_ATOMIC_RMW(I64AtomicRmw8OrU, I64, 0)
WJ_ATOMIC_RMW(I64AtomicRmw8SubU, I64, 0)
WJ_ATOMIC_RMW(I64AtomicRmw8XchgU, I64, 0)
WJ_ATOMIC_RMW(I64AtomicRmw8XorU, I64, 0)
WJ_ATOMIC_RMW(I64AtomicRmwAdd, I64, 3)
WJ_ATOMIC_RMW(I64AtomicRmwAnd, I64, 3)
WJ_ATOMIC_CMPXCHG(I64AtomicRmwCmpxchg, I64, 3)
WJ_ATOMIC_RMW(I64AtomicRmwOr, I64, 3)
WJ_ATOMIC_RMW(I64AtomicRmwSub, I64, 3)
WJ_ATOMIC_RMW(I64AtomicRmwXchg, I64, 3)
WJ_ATOMIC_RMW(I64AtomicRmwXor, I64, 3)
WJ_ATOMIC_STORE(I64AtomicStore, I64, 3)
WJ_ATOMIC_STORE(I64AtomicStore16, I64, 1)
WJ_ATOMIC_STORE(I64AtomicStore32, I64, 2)
WJ_ATOMIC_STORE(I64AtomicStore8, I64, 0)

#undef WJ_ATOMIC_LOAD
#undef WJ_ATOMIC_STORE
#undef WJ_ATOMIC_RMW
#undef WJ_ATOMIC_CMPXCHG

void Validator::dispatchMemoryAtomicNotify(MemArg a) { requireMemoryAccess(a, 2); popExpect(ValueTypeCode::I32); popExpect(memoryAddressType(a.memidx)); pushValue(ValueType{ValueTypeCode::I32, -1}); }
void Validator::dispatchMemoryAtomicWait32(MemArg a) { requireMemoryAccess(a, 2); popExpect(ValueTypeCode::I64); popExpect(ValueTypeCode::I32); popExpect(memoryAddressType(a.memidx)); pushValue(ValueType{ValueTypeCode::I32, -1}); }
void Validator::dispatchMemoryAtomicWait64(MemArg a) { requireMemoryAccess(a, 3); popExpect(ValueTypeCode::I64); popExpect(ValueTypeCode::I64); popExpect(memoryAddressType(a.memidx)); pushValue(ValueType{ValueTypeCode::I32, -1}); }
void Validator::dispatchAtomicFence() {}


// SIMD bitwise / const / bitmask / splat typing.
void Validator::dispatchV128Const(std::span<uint8_t>) { pushValue(ValueType{ValueTypeCode::V128, -1}); }
void Validator::dispatchV128Not() { popExpect(ValueTypeCode::V128); pushValue(ValueType{ValueTypeCode::V128, -1}); }
void Validator::dispatchV128And() { popExpect(ValueTypeCode::V128); popExpect(ValueTypeCode::V128); pushValue(ValueType{ValueTypeCode::V128, -1}); }
void Validator::dispatchV128AndNot() { popExpect(ValueTypeCode::V128); popExpect(ValueTypeCode::V128); pushValue(ValueType{ValueTypeCode::V128, -1}); }
void Validator::dispatchV128Or() { popExpect(ValueTypeCode::V128); popExpect(ValueTypeCode::V128); pushValue(ValueType{ValueTypeCode::V128, -1}); }
void Validator::dispatchV128Xor() { popExpect(ValueTypeCode::V128); popExpect(ValueTypeCode::V128); pushValue(ValueType{ValueTypeCode::V128, -1}); }
void Validator::dispatchV128Bitselect() { popExpect(ValueTypeCode::V128); popExpect(ValueTypeCode::V128); popExpect(ValueTypeCode::V128); pushValue(ValueType{ValueTypeCode::V128, -1}); }
#define WJ_SPLAT(n, T) void Validator::dispatch##n() { popExpect(ValueTypeCode::T); pushValue(ValueType{ValueTypeCode::V128, -1}); }
WJ_SPLAT(I8x16Splat, I32)
WJ_SPLAT(I16x8Splat, I32)
WJ_SPLAT(I32x4Splat, I32)
WJ_SPLAT(I64x2Splat, I64)
WJ_SPLAT(F32x4Splat, F32)
WJ_SPLAT(F64x2Splat, F64)
#undef WJ_SPLAT

void Validator::dispatchI8x16Swizzle() { popExpect(ValueTypeCode::V128); popExpect(ValueTypeCode::V128); pushValue(ValueType{ValueTypeCode::V128, -1}); }
void Validator::dispatchI8x16RelaxedSwizzle() { popExpect(ValueTypeCode::V128); popExpect(ValueTypeCode::V128); pushValue(ValueType{ValueTypeCode::V128, -1}); }

// SIMD arithmetic/comparison/conversion typing: shapes from wasm_simd_ops.inc.
#define WJ_SIMD_BIN(n) \
	void Validator::dispatch##n() { popExpect(ValueTypeCode::V128); popExpect(ValueTypeCode::V128); pushValue(ValueType{ValueTypeCode::V128, -1}); }
#define WJ_SIMD_UN(n) \
	void Validator::dispatch##n() { popExpect(ValueTypeCode::V128); pushValue(ValueType{ValueTypeCode::V128, -1}); }
#define WJ_SIMD_SHIFT(n) \
	void Validator::dispatch##n() { popExpect(ValueTypeCode::I32); popExpect(ValueTypeCode::V128); pushValue(ValueType{ValueTypeCode::V128, -1}); }
#define WJ_SIMD_RED(n) \
	void Validator::dispatch##n() { popExpect(ValueTypeCode::V128); pushValue(ValueType{ValueTypeCode::I32, -1}); }

WJ_SIMD_BIN(I8x16Add)
WJ_SIMD_BIN(I8x16Sub)
WJ_SIMD_BIN(I8x16MinS)
WJ_SIMD_BIN(I8x16MinU)
WJ_SIMD_BIN(I8x16MaxS)
WJ_SIMD_BIN(I8x16MaxU)
WJ_SIMD_BIN(I16x8Add)
WJ_SIMD_BIN(I16x8Sub)
WJ_SIMD_BIN(I16x8Mul)
WJ_SIMD_BIN(I16x8MinS)
WJ_SIMD_BIN(I16x8MinU)
WJ_SIMD_BIN(I16x8MaxS)
WJ_SIMD_BIN(I16x8MaxU)
WJ_SIMD_BIN(I32x4Add)
WJ_SIMD_BIN(I32x4Sub)
WJ_SIMD_BIN(I32x4Mul)
WJ_SIMD_BIN(I32x4MinS)
WJ_SIMD_BIN(I32x4MinU)
WJ_SIMD_BIN(I32x4MaxS)
WJ_SIMD_BIN(I32x4MaxU)
WJ_SIMD_BIN(I64x2Add)
WJ_SIMD_BIN(I64x2Sub)
WJ_SIMD_BIN(I64x2Mul)
WJ_SIMD_BIN(I8x16Eq)
WJ_SIMD_BIN(I8x16Ne)
WJ_SIMD_BIN(I8x16LtS)
WJ_SIMD_BIN(I8x16LtU)
WJ_SIMD_BIN(I8x16GtS)
WJ_SIMD_BIN(I8x16GtU)
WJ_SIMD_BIN(I8x16LeS)
WJ_SIMD_BIN(I8x16LeU)
WJ_SIMD_BIN(I8x16GeS)
WJ_SIMD_BIN(I8x16GeU)
WJ_SIMD_BIN(I16x8Eq)
WJ_SIMD_BIN(I16x8Ne)
WJ_SIMD_BIN(I16x8LtS)
WJ_SIMD_BIN(I16x8LtU)
WJ_SIMD_BIN(I16x8GtS)
WJ_SIMD_BIN(I16x8GtU)
WJ_SIMD_BIN(I16x8LeS)
WJ_SIMD_BIN(I16x8LeU)
WJ_SIMD_BIN(I16x8GeS)
WJ_SIMD_BIN(I16x8GeU)
WJ_SIMD_BIN(I32x4Eq)
WJ_SIMD_BIN(I32x4Ne)
WJ_SIMD_BIN(I32x4LtS)
WJ_SIMD_BIN(I32x4LtU)
WJ_SIMD_BIN(I32x4GtS)
WJ_SIMD_BIN(I32x4GtU)
WJ_SIMD_BIN(I32x4LeS)
WJ_SIMD_BIN(I32x4LeU)
WJ_SIMD_BIN(I32x4GeS)
WJ_SIMD_BIN(I32x4GeU)
WJ_SIMD_BIN(I64x2Eq)
WJ_SIMD_BIN(I64x2Ne)
WJ_SIMD_BIN(I64x2LtS)
WJ_SIMD_BIN(I64x2GtS)
WJ_SIMD_BIN(I64x2LeS)
WJ_SIMD_BIN(I64x2GeS)
WJ_SIMD_UN(I8x16Neg)
WJ_SIMD_UN(I16x8Neg)
WJ_SIMD_UN(I32x4Neg)
WJ_SIMD_UN(I64x2Neg)
WJ_SIMD_UN(I8x16Abs)
WJ_SIMD_UN(I16x8Abs)
WJ_SIMD_UN(I32x4Abs)
WJ_SIMD_UN(I64x2Abs)
WJ_SIMD_SHIFT(I8x16Shl)
WJ_SIMD_SHIFT(I8x16ShrS)
WJ_SIMD_SHIFT(I8x16ShrU)
WJ_SIMD_SHIFT(I16x8Shl)
WJ_SIMD_SHIFT(I16x8ShrS)
WJ_SIMD_SHIFT(I16x8ShrU)
WJ_SIMD_SHIFT(I32x4Shl)
WJ_SIMD_SHIFT(I32x4ShrS)
WJ_SIMD_SHIFT(I32x4ShrU)
WJ_SIMD_SHIFT(I64x2Shl)
WJ_SIMD_SHIFT(I64x2ShrS)
WJ_SIMD_SHIFT(I64x2ShrU)
WJ_SIMD_BIN(I8x16AddSatS)
WJ_SIMD_BIN(I8x16AddSatU)
WJ_SIMD_BIN(I8x16SubSatS)
WJ_SIMD_BIN(I8x16SubSatU)
WJ_SIMD_BIN(I16x8AddSatS)
WJ_SIMD_BIN(I16x8AddSatU)
WJ_SIMD_BIN(I16x8SubSatS)
WJ_SIMD_BIN(I16x8SubSatU)
WJ_SIMD_BIN(I8x16AvgrU)
WJ_SIMD_BIN(I16x8AvgrU)
WJ_SIMD_UN(I8x16Popcnt)
WJ_SIMD_BIN(I16x8Q15MulRSatS)
WJ_SIMD_RED(I8x16AllTrue)
WJ_SIMD_RED(I16x8AllTrue)
WJ_SIMD_RED(I32x4AllTrue)
WJ_SIMD_RED(I64x2AllTrue)
WJ_SIMD_RED(V128AnyTrue)
WJ_SIMD_RED(I8x16Bitmask)
WJ_SIMD_RED(I16x8Bitmask)
WJ_SIMD_RED(I32x4Bitmask)
WJ_SIMD_RED(I64x2Bitmask)
WJ_SIMD_UN(I16x8ExtendLowI8x16S)
WJ_SIMD_UN(I16x8ExtendHighI8x16S)
WJ_SIMD_UN(I16x8ExtendLowI8x16U)
WJ_SIMD_UN(I16x8ExtendHighI8x16U)
WJ_SIMD_UN(I32x4ExtendLowI16x8S)
WJ_SIMD_UN(I32x4ExtendHighI16x8S)
WJ_SIMD_UN(I32x4ExtendLowI16x8U)
WJ_SIMD_UN(I32x4ExtendHighI16x8U)
WJ_SIMD_UN(I64x2ExtendLowI32x4S)
WJ_SIMD_UN(I64x2ExtendHighI32x4S)
WJ_SIMD_UN(I64x2ExtendLowI32x4U)
WJ_SIMD_UN(I64x2ExtendHighI32x4U)
WJ_SIMD_BIN(I16x8ExtMulLowI8x16S)
WJ_SIMD_BIN(I16x8ExtMulHighI8x16S)
WJ_SIMD_BIN(I16x8ExtMulLowI8x16U)
WJ_SIMD_BIN(I16x8ExtMulHighI8x16U)
WJ_SIMD_BIN(I32x4ExtMulLowI16x8S)
WJ_SIMD_BIN(I32x4ExtMulHighI16x8S)
WJ_SIMD_BIN(I32x4ExtMulLowI16x8U)
WJ_SIMD_BIN(I32x4ExtMulHighI16x8U)
WJ_SIMD_BIN(I64x2ExtMulLowI32x4S)
WJ_SIMD_BIN(I64x2ExtMulHighI32x4S)
WJ_SIMD_BIN(I64x2ExtMulLowI32x4U)
WJ_SIMD_BIN(I64x2ExtMulHighI32x4U)
WJ_SIMD_UN(I16x8ExtAddPairwiseI8x16S)
WJ_SIMD_UN(I16x8ExtAddPairwiseI8x16U)
WJ_SIMD_UN(I32x4ExtAddPairwiseI16x8S)
WJ_SIMD_UN(I32x4ExtAddPairwiseI16x8U)
WJ_SIMD_BIN(I32x4DotI16x8S)
WJ_SIMD_BIN(I8x16NarrowI16x8S)
WJ_SIMD_BIN(I8x16NarrowI16x8U)
WJ_SIMD_BIN(I16x8NarrowI32x4S)
WJ_SIMD_BIN(I16x8NarrowI32x4U)
WJ_SIMD_BIN(F32x4Eq)
WJ_SIMD_BIN(F32x4Ne)
WJ_SIMD_BIN(F32x4Lt)
WJ_SIMD_BIN(F32x4Gt)
WJ_SIMD_BIN(F32x4Le)
WJ_SIMD_BIN(F32x4Ge)
WJ_SIMD_BIN(F64x2Eq)
WJ_SIMD_BIN(F64x2Ne)
WJ_SIMD_BIN(F64x2Lt)
WJ_SIMD_BIN(F64x2Gt)
WJ_SIMD_BIN(F64x2Le)
WJ_SIMD_BIN(F64x2Ge)
WJ_SIMD_UN(F32x4Ceil)
WJ_SIMD_UN(F32x4Floor)
WJ_SIMD_UN(F32x4Trunc)
WJ_SIMD_UN(F32x4Nearest)
WJ_SIMD_UN(F32x4Abs)
WJ_SIMD_UN(F32x4Neg)
WJ_SIMD_UN(F32x4Sqrt)
WJ_SIMD_UN(F64x2Ceil)
WJ_SIMD_UN(F64x2Floor)
WJ_SIMD_UN(F64x2Trunc)
WJ_SIMD_UN(F64x2Nearest)
WJ_SIMD_UN(F64x2Abs)
WJ_SIMD_UN(F64x2Neg)
WJ_SIMD_UN(F64x2Sqrt)
WJ_SIMD_BIN(F32x4Add)
WJ_SIMD_BIN(F32x4Sub)
WJ_SIMD_BIN(F32x4Mul)
WJ_SIMD_BIN(F32x4Div)
WJ_SIMD_BIN(F32x4Min)
WJ_SIMD_BIN(F32x4Max)
WJ_SIMD_BIN(F32x4PMin)
WJ_SIMD_BIN(F32x4PMax)
WJ_SIMD_BIN(F64x2Add)
WJ_SIMD_BIN(F64x2Sub)
WJ_SIMD_BIN(F64x2Mul)
WJ_SIMD_BIN(F64x2Div)
WJ_SIMD_BIN(F64x2Min)
WJ_SIMD_BIN(F64x2Max)
WJ_SIMD_BIN(F64x2PMin)
WJ_SIMD_BIN(F64x2PMax)
WJ_SIMD_UN(I32x4TruncSatF32x4S)
WJ_SIMD_UN(I32x4TruncSatF32x4U)
WJ_SIMD_UN(F32x4ConvertI32x4S)
WJ_SIMD_UN(F32x4ConvertI32x4U)
WJ_SIMD_UN(F64x2ConvertLowI32x4S)
WJ_SIMD_UN(F64x2ConvertLowI32x4U)
WJ_SIMD_UN(F64x2PromoteLowF32x4)
WJ_SIMD_UN(I32x4TruncSatF64x2SZero)
WJ_SIMD_UN(I32x4TruncSatF64x2UZero)
WJ_SIMD_UN(F32x4DemoteF64x2Zero)
WJ_SIMD_UN(I32x4RelaxedTruncF32x4S)
WJ_SIMD_UN(I32x4RelaxedTruncF32x4U)
WJ_SIMD_UN(I32x4RelaxedTruncF64x2SZero)
WJ_SIMD_UN(I32x4RelaxedTruncF64x2UZero)
WJ_SIMD_BIN(F32x4RelaxedMin)
WJ_SIMD_BIN(F32x4RelaxedMax)
WJ_SIMD_BIN(F64x2RelaxedMin)
WJ_SIMD_BIN(F64x2RelaxedMax)
WJ_SIMD_BIN(I16x8RelaxedQ15MulRS)

#undef WJ_SIMD_BIN
#undef WJ_SIMD_UN
#undef WJ_SIMD_SHIFT
#undef WJ_SIMD_RED

// Core numeric instructions: fixed signatures, one line each.
#define WJ_TEST(name, T) \
	void Validator::dispatch##name() { popExpect(ValueTypeCode::T); pushValue(ValueType{ValueTypeCode::I32, -1}); }
#define WJ_UNOP(name, T) \
	void Validator::dispatch##name() { popExpect(ValueTypeCode::T); pushValue(ValueType{ValueTypeCode::T, -1}); }
#define WJ_BINOP(name, T) \
	void Validator::dispatch##name() { popExpect(ValueTypeCode::T); popExpect(ValueTypeCode::T); pushValue(ValueType{ValueTypeCode::T, -1}); }
#define WJ_RELOP(name, T) \
	void Validator::dispatch##name() { popExpect(ValueTypeCode::T); popExpect(ValueTypeCode::T); pushValue(ValueType{ValueTypeCode::I32, -1}); }
#define WJ_CONV(name, From, To) \
	void Validator::dispatch##name() { popExpect(ValueTypeCode::From); pushValue(ValueType{ValueTypeCode::To, -1}); }

WJ_TEST(I32Eqz, I32)
WJ_TEST(I64Eqz, I64)
WJ_UNOP(F32Abs, F32)
WJ_UNOP(F32Ceil, F32)
WJ_UNOP(F32Floor, F32)
WJ_UNOP(F32Nearest, F32)
WJ_UNOP(F32Neg, F32)
WJ_UNOP(F32Sqrt, F32)
WJ_UNOP(F32Trunc, F32)
WJ_UNOP(F64Abs, F64)
WJ_UNOP(F64Ceil, F64)
WJ_UNOP(F64Floor, F64)
WJ_UNOP(F64Nearest, F64)
WJ_UNOP(F64Neg, F64)
WJ_UNOP(F64Sqrt, F64)
WJ_UNOP(F64Trunc, F64)
WJ_UNOP(I32Clz, I32)
WJ_UNOP(I32Ctz, I32)
WJ_UNOP(I32Extend16S, I32)
WJ_UNOP(I32Extend8S, I32)
WJ_UNOP(I32Popcnt, I32)
WJ_UNOP(I64Clz, I64)
WJ_UNOP(I64Ctz, I64)
WJ_UNOP(I64Extend16S, I64)
WJ_UNOP(I64Extend32S, I64)
WJ_UNOP(I64Extend8S, I64)
WJ_UNOP(I64Popcnt, I64)
WJ_RELOP(F32Eq, F32)
WJ_RELOP(F32Ge, F32)
WJ_RELOP(F32Gt, F32)
WJ_RELOP(F32Le, F32)
WJ_RELOP(F32Lt, F32)
WJ_RELOP(F32Ne, F32)
WJ_RELOP(F64Eq, F64)
WJ_RELOP(F64Ge, F64)
WJ_RELOP(F64Gt, F64)
WJ_RELOP(F64Le, F64)
WJ_RELOP(F64Lt, F64)
WJ_RELOP(F64Ne, F64)
WJ_RELOP(I32Eq, I32)
WJ_RELOP(I32GeS, I32)
WJ_RELOP(I32GeU, I32)
WJ_RELOP(I32GtS, I32)
WJ_RELOP(I32GtU, I32)
WJ_RELOP(I32LeS, I32)
WJ_RELOP(I32LeU, I32)
WJ_RELOP(I32LtS, I32)
WJ_RELOP(I32LtU, I32)
WJ_RELOP(I32Ne, I32)
WJ_RELOP(I64Eq, I64)
WJ_RELOP(I64GeS, I64)
WJ_RELOP(I64GeU, I64)
WJ_RELOP(I64GtS, I64)
WJ_RELOP(I64GtU, I64)
WJ_RELOP(I64LeS, I64)
WJ_RELOP(I64LeU, I64)
WJ_RELOP(I64LtS, I64)
WJ_RELOP(I64LtU, I64)
WJ_RELOP(I64Ne, I64)
WJ_BINOP(F32Add, F32)
WJ_BINOP(F32Copysign, F32)
WJ_BINOP(F32Div, F32)
WJ_BINOP(F32Max, F32)
WJ_BINOP(F32Min, F32)
WJ_BINOP(F32Mul, F32)
WJ_BINOP(F32Sub, F32)
WJ_BINOP(F64Add, F64)
WJ_BINOP(F64Copysign, F64)
WJ_BINOP(F64Div, F64)
WJ_BINOP(F64Max, F64)
WJ_BINOP(F64Min, F64)
WJ_BINOP(F64Mul, F64)
WJ_BINOP(F64Sub, F64)
WJ_BINOP(I32Add, I32)
WJ_BINOP(I32And, I32)
WJ_BINOP(I32DivS, I32)
WJ_BINOP(I32DivU, I32)
WJ_BINOP(I32Mul, I32)
WJ_BINOP(I32Or, I32)
WJ_BINOP(I32RemS, I32)
WJ_BINOP(I32RemU, I32)
WJ_BINOP(I32Rotl, I32)
WJ_BINOP(I32Rotr, I32)
WJ_BINOP(I32Shl, I32)
WJ_BINOP(I32ShrS, I32)
WJ_BINOP(I32ShrU, I32)
WJ_BINOP(I32Sub, I32)
WJ_BINOP(I32Xor, I32)
WJ_BINOP(I64Add, I64)
WJ_BINOP(I64And, I64)
WJ_BINOP(I64DivS, I64)
WJ_BINOP(I64DivU, I64)
WJ_BINOP(I64Mul, I64)
WJ_BINOP(I64Or, I64)
WJ_BINOP(I64RemS, I64)
WJ_BINOP(I64RemU, I64)
WJ_BINOP(I64Rotl, I64)
WJ_BINOP(I64Rotr, I64)
WJ_BINOP(I64Shl, I64)
WJ_BINOP(I64ShrS, I64)
WJ_BINOP(I64ShrU, I64)
WJ_BINOP(I64Sub, I64)
WJ_BINOP(I64Xor, I64)
WJ_CONV(F32ConvertI32S, I32, F32)
WJ_CONV(F32ConvertI32U, I32, F32)
WJ_CONV(F32ConvertI64S, I64, F32)
WJ_CONV(F32ConvertI64U, I64, F32)
WJ_CONV(F32DemoteF64, F64, F32)
WJ_CONV(F32ReinterpretI32, I32, F32)
WJ_CONV(F64ConvertI32S, I32, F64)
WJ_CONV(F64ConvertI32U, I32, F64)
WJ_CONV(F64ConvertI64S, I64, F64)
WJ_CONV(F64ConvertI64U, I64, F64)
WJ_CONV(F64PromoteF32, F32, F64)
WJ_CONV(F64ReinterpretI64, I64, F64)
WJ_CONV(I32ReinterpretF32, F32, I32)
WJ_CONV(I32TruncF32S, F32, I32)
WJ_CONV(I32TruncF32U, F32, I32)
WJ_CONV(I32TruncF64S, F64, I32)
WJ_CONV(I32TruncF64U, F64, I32)
WJ_CONV(I32TruncSatF32S, F32, I32)
WJ_CONV(I32TruncSatF32U, F32, I32)
WJ_CONV(I32TruncSatF64S, F64, I32)
WJ_CONV(I32TruncSatF64U, F64, I32)
WJ_CONV(I32WrapI64, I64, I32)
WJ_CONV(I64ExtendI32S, I32, I64)
WJ_CONV(I64ExtendI32U, I32, I64)
WJ_CONV(I64ReinterpretF64, F64, I64)
WJ_CONV(I64TruncF32S, F32, I64)
WJ_CONV(I64TruncF32U, F32, I64)
WJ_CONV(I64TruncF64S, F64, I64)
WJ_CONV(I64TruncF64U, F64, I64)
WJ_CONV(I64TruncSatF32S, F32, I64)
WJ_CONV(I64TruncSatF32U, F32, I64)
WJ_CONV(I64TruncSatF64S, F64, I64)
WJ_CONV(I64TruncSatF64U, F64, I64)

#undef WJ_TEST
#undef WJ_UNOP
#undef WJ_BINOP
#undef WJ_RELOP
#undef WJ_CONV

// ── Stringref core instruction typing (docs/proposals/STRINGREF.md STR-3) ──

void Validator::dispatchStringNewUtf8(MemIdx arg)
{
	requireMemoryIndex(arg);
	popExpect(ValueTypeCode::I32); // len
	popExpect(ValueTypeCode::I32); // ptr
	pushValue(ValueType{ValueTypeCode::StringRef, -1});
}

void Validator::dispatchStringNewWtf16(MemIdx arg)
{
	requireMemoryIndex(arg);
	popExpect(ValueTypeCode::I32); // code units
	popExpect(ValueTypeCode::I32); // ptr
	pushValue(ValueType{ValueTypeCode::StringRef, -1});
}

void Validator::dispatchStringNewLossyUtf8(MemIdx arg)
{
	requireMemoryIndex(arg);
	popExpect(ValueTypeCode::I32);
	popExpect(ValueTypeCode::I32);
	pushValue(ValueType{ValueTypeCode::StringRef, -1});
}

void Validator::dispatchStringNewWtf8(MemIdx arg)
{
	requireMemoryIndex(arg);
	popExpect(ValueTypeCode::I32);
	popExpect(ValueTypeCode::I32);
	pushValue(ValueType{ValueTypeCode::StringRef, -1});
}

void Validator::dispatchStringConst(uint32_t idx)
{
	if (idx >= module.stringLiterals.size())
		fail("string.const index " + std::to_string(idx) + " is out of range");
	pushValue(ValueType{ValueTypeCode::StringRef, -1});
}

void Validator::dispatchStringMeasureUtf8()
{
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringMeasureWtf8()
{
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringMeasureWtf16()
{
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringEncodeUtf8(MemIdx arg)
{
	requireMemoryIndex(arg);
	popExpect(ValueTypeCode::I32); // ptr
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringEncodeLossyUtf8(MemIdx arg)
{
	requireMemoryIndex(arg);
	popExpect(ValueTypeCode::I32);
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringEncodeWtf8(MemIdx arg)
{
	requireMemoryIndex(arg);
	popExpect(ValueTypeCode::I32);
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringEncodeWtf16(MemIdx arg)
{
	requireMemoryIndex(arg);
	popExpect(ValueTypeCode::I32);
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringConcat()
{
	popExpect(ValueTypeCode::StringRef);
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::StringRef, -1});
}

void Validator::dispatchStringEq()
{
	popExpect(ValueTypeCode::StringRef);
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringIsUsvSequence()
{
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

// ── stringview typing (docs/proposals/STRINGREF.md STR-4) ──────────────────

void Validator::dispatchStringAsWtf8()
{
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::StringViewWtf8, -1});
}

void Validator::dispatchStringAsWtf16()
{
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::StringViewWtf16, -1});
}

void Validator::dispatchStringAsIter()
{
	popExpect(ValueTypeCode::StringRef);
	pushValue(ValueType{ValueTypeCode::StringViewIter, -1});
}

void Validator::dispatchStringViewWtf8Advance()
{
	popExpect(ValueTypeCode::I32); // bytes
	popExpect(ValueTypeCode::I32); // pos
	popExpect(ValueTypeCode::StringViewWtf8);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringViewWtf8EncodeUtf8(MemIdx arg)
{
	requireMemoryIndex(arg);
	popExpect(ValueTypeCode::I32); // bytes
	popExpect(ValueTypeCode::I32); // pos
	popExpect(ValueTypeCode::I32); // ptr
	popExpect(ValueTypeCode::StringViewWtf8);
	pushValue(ValueType{ValueTypeCode::I32, -1}); // next_pos
	pushValue(ValueType{ValueTypeCode::I32, -1}); // written
}

void Validator::dispatchStringViewWtf8EncodeLossyUtf8(MemIdx arg)
{
	dispatchStringViewWtf8EncodeUtf8(arg);
}

void Validator::dispatchStringViewWtf8EncodeWtf8(MemIdx arg)
{
	dispatchStringViewWtf8EncodeUtf8(arg);
}

void Validator::dispatchStringViewWtf8Slice()
{
	popExpect(ValueTypeCode::I32); // end
	popExpect(ValueTypeCode::I32); // start
	popExpect(ValueTypeCode::StringViewWtf8);
	pushValue(ValueType{ValueTypeCode::StringRef, -1});
}

void Validator::dispatchStringViewWtf16Length()
{
	popExpect(ValueTypeCode::StringViewWtf16);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringViewWtf16GetCodeUnit()
{
	popExpect(ValueTypeCode::I32); // pos
	popExpect(ValueTypeCode::StringViewWtf16);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringViewWtf16Encode(MemIdx arg)
{
	requireMemoryIndex(arg);
	popExpect(ValueTypeCode::I32); // units
	popExpect(ValueTypeCode::I32); // pos
	popExpect(ValueTypeCode::I32); // ptr
	popExpect(ValueTypeCode::StringViewWtf16);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringViewWtf16Slice()
{
	popExpect(ValueTypeCode::I32); // end
	popExpect(ValueTypeCode::I32); // start
	popExpect(ValueTypeCode::StringViewWtf16);
	pushValue(ValueType{ValueTypeCode::StringRef, -1});
}

void Validator::dispatchStringViewIterNext()
{
	popExpect(ValueTypeCode::StringViewIter);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringViewIterAdvance()
{
	popExpect(ValueTypeCode::I32); // count
	popExpect(ValueTypeCode::StringViewIter);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringViewIterRewind()
{
	popExpect(ValueTypeCode::I32); // count
	popExpect(ValueTypeCode::StringViewIter);
	pushValue(ValueType{ValueTypeCode::I32, -1});
}

void Validator::dispatchStringViewIterSlice()
{
	popExpect(ValueTypeCode::I32); // count
	popExpect(ValueTypeCode::StringViewIter);
	pushValue(ValueType{ValueTypeCode::StringRef, -1});
}

} // namespace WASM
