#include "WasmValidator.hpp"
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
	const std::string prefix = currentBody == kNoBody
			? std::string()
			: "function " + std::to_string(currentBody) + " at instruction "
					+ std::to_string(instructionOffset) + ": ";
	throw ValidationError(prefix + message);
}

const Subtype& Validator::subtypeAt(uint32_t typeIdx) const
{
	if (typeIdx >= module.types.size())
		fail("unknown type " + std::to_string(typeIdx));
	return module.types[typeIdx];
}

const FuncType& Validator::funcTypeAt(uint32_t typeIdx) const
{
	const Subtype& subtype = subtypeAt(typeIdx);
	if (!subtype.isFunction())
		fail("type " + std::to_string(typeIdx) + " is not a function type");
	return std::get<FuncType>(subtype.composite);
}

void Validator::requireFunctionIndex(uint32_t idx) const
{
	if (idx >= totalFunctions)
		fail("unknown function " + std::to_string(idx));
}

void Validator::requireTableIndex(uint32_t idx) const
{
	if (idx >= totalTables)
		fail("unknown table " + std::to_string(idx));
}

void Validator::requireMemoryIndex(uint32_t idx) const
{
	if (idx >= totalMemories)
		fail("unknown memory " + std::to_string(idx));
}

void Validator::requireGlobalIndex(uint32_t idx) const
{
	if (idx >= totalGlobals)
		fail("unknown global " + std::to_string(idx));
}

void Validator::requireTagIndex(uint32_t idx) const
{
	if (idx >= totalTags)
		fail("unknown tag " + std::to_string(idx));
}

void Validator::requireLocalIndex(uint32_t idx) const
{
	if (idx >= locals.size())
		fail("unknown local " + std::to_string(idx));
}

void Validator::requireElementIndex(uint32_t idx) const
{
	if (idx >= module.elementSegments.size())
		fail("unknown element segment " + std::to_string(idx));
}

void Validator::requireDataIndex(uint32_t idx) const
{
	if (idx >= module.dataSegments.size())
		fail("unknown data segment " + std::to_string(idx));
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
			return typeIdx < module.types.size() && module.types[typeIdx].isFunction();
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
	// A value-typed block type is a single *result*; it never has parameters.
	return {};
}

std::vector<ValueType> Validator::blockResults(const BlockType& blockType) const
{
	if (blockType.isTypeIndex)
		return resultTypes(funcTypeAt(blockType.typeIndex));
	if (blockType.isVoid())
		return {};
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
	if (actual.opcode == expected.opcode)
		return true;
	const auto isRef = [](ValueTypeCode code) {
		return code == ValueTypeCode::FuncRef || code == ValueTypeCode::ExternRef
				|| code == ValueTypeCode::AnyRef || code == ValueTypeCode::EqRef
				|| code == ValueTypeCode::I31Ref || code == ValueTypeCode::StructRef
				|| code == ValueTypeCode::ArrayRef || code == ValueTypeCode::ExnRef
				|| code == ValueTypeCode::NullFuncRef || code == ValueTypeCode::NullExternRef
				|| code == ValueTypeCode::NullExnRef || code == ValueTypeCode::NullRef
				|| code == ValueTypeCode::Ref || code == ValueTypeCode::RefNull;
	};
	if (!isRef(actual.opcode) || !isRef(expected.opcode))
		return false;
	if (actual.heapType >= 0 && expected.heapType >= 0) {
		return actual.heapType == expected.heapType
				|| module.isSubtype(static_cast<TypeIdx>(actual.heapType),
									static_cast<TypeIdx>(expected.heapType));
	}
	return true; // nullability / abstract-hierarchy differences: stay permissive
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
	controlStack.push_back(ControlFrame{params, results, height, isLoop, isIf, false, false});
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
		typeStack.resize(current.height);
		pushTypes(current.startTypes);
		current.unreachable = false;
		current.elseSeen = true;
		return;
	}
	popTypes(current.endTypes);
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

const FuncType& Validator::functionType(FuncIdx idx) const
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
	for (const ImportTag& imp : module.importTags) {
		if (!funcTypeAt(imp.typeIdx).results.empty())
			fail("non-empty tag result type");
	}
	for (const Tag& tag : module.tags) {
		if (!funcTypeAt(tag.typeIdx).results.empty())
			fail("non-empty tag result type");
	}

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
			fail("start function must have type [] -> []");
	}

	std::unordered_set<std::string> exportNames;
	for (const Export& ex : module.exports) {
		if (!exportNames.insert(ex.name).second)
			fail("duplicate export name '" + ex.name + "'");
		switch (ex.kind) {
		case ExternalKind::Function: requireFunctionIndex(ex.index); break;
		case ExternalKind::Table:    requireTableIndex(ex.index);    break;
		case ExternalKind::Memory:   requireMemoryIndex(ex.index);   break;
		case ExternalKind::Global:   requireGlobalIndex(ex.index);   break;
		case ExternalKind::Tag:      requireTagIndex(ex.index);      break;
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
	}
	for (std::size_t i = 0; i < module.tables.size(); ++i) {
		const TableType& tt = module.tables[i];
		checkLimits(tt.limits, (tt.limits.flags & 0x04) != 0, false, "table", i);
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
			if (ht.isTypeIndex && ht.typeIndex >= module.types.size())
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
			push(ValueTypeCode::FuncRef);
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
				return; // not a modelled initializer form: accept
			device.seek(16, Elv::Io::SeekOrigin::CUR);
			push(ValueTypeCode::V128);
			break;
		}
		case 0x0B: // end
			done = true;
			break;
		default:
			// A const form this milestone does not model (v128.const, GC
			// constructors, …): accept the initializer rather than guess.
			return;
		}
	}

	if (stack.size() != 1)
		fail(label + ": type mismatch in initializer");
	const ValueType& got = stack.back();
	const ValueType& want = global.type.contentType;
	const auto isRef = [](ValueTypeCode code) {
		return code == ValueTypeCode::FuncRef || code == ValueTypeCode::ExternRef
				|| code == ValueTypeCode::AnyRef || code == ValueTypeCode::EqRef
				|| code == ValueTypeCode::I31Ref || code == ValueTypeCode::StructRef
				|| code == ValueTypeCode::ArrayRef || code == ValueTypeCode::ExnRef
				|| code == ValueTypeCode::NullFuncRef || code == ValueTypeCode::NullExternRef
				|| code == ValueTypeCode::NullExnRef || code == ValueTypeCode::NullRef
				|| code == ValueTypeCode::Ref || code == ValueTypeCode::RefNull;
	};
	if (got.opcode != want.opcode && !(isRef(got.opcode) && isRef(want.opcode)))
		fail(label + ": initializer type does not match the global type");
}

void Validator::validateElementSegment(const ElementSegment& segment, std::size_t index)
{
	if (segment.isActive() && segment.tableIdx >= totalTables)
		fail("element segment " + std::to_string(index) + ": unknown table");
	for (uint32_t funcIdx : segment.initIndices)
		requireFunctionIndex(funcIdx);
}

void Validator::validateDataSegment(const DataSegment& segment, std::size_t index)
{
	// Mode bit 0: 0 = active, 1 = passive.
	if ((segment.mode & 0x01) == 0 && segment.memoryIdx >= totalMemories)
		fail("data segment " + std::to_string(index) + ": unknown memory");
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
	controlStack.push_back(ControlFrame{{}, resultTypes(funcType), 0, false, false, false, false});

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
	pushValue(locals[arg]);
}

void Validator::dispatchLocalSet(LocalIdx arg)
{
	requireLocalIndex(arg);
	popExpect(locals[arg]);
}

void Validator::dispatchLocalTee(LocalIdx arg)
{
	requireLocalIndex(arg);
	popExpect(locals[arg]);
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
		fail("global.set of an immutable global");
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
		fail("call_indirect on a non-funcref table");
	popExpect(tableIndexType(tableIdx));
	popTypes(paramTypes(type));
	pushTypes(resultTypes(type));
}

void Validator::dispatchReturnCallIndirect(TypeIdx typeIdx, TableIdx tableIdx)
{
	const FuncType& type = funcTypeAt(typeIdx);
	requireTableIndex(tableIdx);
	if (!tableIsFuncRef(tableIdx))
		fail("return_call_indirect on a non-funcref table");
	popExpect(tableIndexType(tableIdx));
	popTypes(paramTypes(type));
	checkTailCallResults(type);
	setUnreachable();
}

void Validator::dispatchCallRef(TypeIdx typeIdx)
{
	const FuncType& type = funcTypeAt(typeIdx);
	popExpect(ValueType{ValueTypeCode::Ref, -1});
	popTypes(paramTypes(type));
	pushTypes(resultTypes(type));
}

void Validator::dispatchReturnCallRef(TypeIdx typeIdx)
{
	const FuncType& type = funcTypeAt(typeIdx);
	popExpect(ValueType{ValueTypeCode::Ref, -1});
	popTypes(paramTypes(type));
	checkTailCallResults(type);
	setUnreachable();
}

void Validator::dispatchRefFunc(FuncIdx arg)
{
	requireFunctionIndex(arg);
	pushValue(ValueType{ValueTypeCode::FuncRef, -1});
}

void Validator::dispatchRefNull(const HeapType& heapType)
{
	if (heapType.isTypeIndex)
		subtypeAt(heapType.typeIndex);
	ValueType type{};
	type.opcode = ValueTypeCode::RefNull;
	type.heapType = heapType.isTypeIndex
			? static_cast<int32_t>(heapType.typeIndex)
			: static_cast<int32_t>(heapType.abstract);
	pushValue(type);
}

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

void Validator::dispatchTryTable(const BlockType& blockType, std::vector<CatchClause>&& catches)
{
	for (const CatchClause& clause : catches) {
		if (clause.kind == CatchKind::Catch || clause.kind == CatchKind::CatchRef)
			requireTagIndex(clause.tagIdx);
	}
	// Catch labels are checked once the tag payload rules land; the block
	// itself is typed like any other block. A catch also injects the payload
	// into an *outer* label, which this linear stack does not model, so stop
	// trusting the operand stack from here (indices are still checked).
	beginBlock(blockType, false, false);
	typeTainted = true;
}

void Validator::dispatchSelectT(std::vector<ValueType>&& types)
{
	if (types.size() != 1)
		fail("select: invalid result arity");
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
	const auto typesFor = [this](LabelIdx label) -> const std::vector<ValueType>& {
		const ControlFrame& target = controlStack[controlStack.size() - 1 - label];
		return target.isLoop ? target.startTypes : target.endTypes;
	};
	const std::vector<ValueType> defaultTypes = typesFor(defaultLabel);
	for (LabelIdx label : labels) {
		if (label >= controlStack.size())
			fail("unknown label " + std::to_string(label));
		const std::vector<ValueType>& types = typesFor(label);
		if (types.size() != defaultTypes.size())
			fail("type mismatch: br_table target arity differs");
		for (std::size_t i = 0; i < types.size(); ++i) {
			if (!assignable(types[i], defaultTypes[i]))
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
	(void)popAny();
	(void)popAny();
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

} // namespace WASM
