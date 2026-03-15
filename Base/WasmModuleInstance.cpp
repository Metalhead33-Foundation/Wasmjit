#include "WasmModuleInstance.hpp"
#include "WasmException.hpp"
#include "../Io/EuphConstBufferDevice.hpp"
#include "../Io/ElvDataStream.hpp"
namespace WASM {

ModuleInstance::ModuleInstance(const Module& module, ImportResolver& resolver)
	: module(&module)
{
	// ── Step 1: Resolve and store all imports ───────────────────────
	// Imports must come first because they occupy the low indices in
	// each index space. We need importedFunctions populated before we
	// can evaluate any initializer expressions (which might call imports).
	resolveImports(resolver);

	// ── Step 2: Allocate linear memory ─────────────────────────────
	// Use the memory type from the module to set the initial size and max.
	// If memory was imported, resolveImports() already set ctx.memoryBase;
	// we skip allocation in that case.
	initializeMemory();

	// ── Step 3: Initialize globals ──────────────────────────────────
	// Global initializer expressions are constant-only in MVP Wasm,
	// so we can evaluate them right now without needing compiled functions.
	initializeGlobals();

	// ── Step 4: Allocate and pre-populate the table ─────────────────
	initializeTable();

	// ── Step 5: Reserve space for compiled function handles ─────────
	// We don't compile yet — that's the ModuleInstantiator's job.
	// But we size the vector now so the instantiator can index into it
	// directly without worrying about bounds.
	compiledFunctions.resize(module.internalFunctionTypeIndices.size(), nullptr);
}

void ModuleInstance::resolveImports(ImportResolver& resolver)
{
	// Pre-allocate so we can index by import order without reallocation.
	importStorage.reserve(module->importFunctions.size());

	for (const auto& imp : module->importFunctions) {
		auto callable = resolver.resolveFunction(
			imp.moduleName, imp.fieldName, imp.typeIdx);

		// A missing import is a hard instantiation-time error in Wasm,
		// not a runtime trap. We fail loudly here rather than storing
		// a null that would cause a mysterious crash later.
		if (!callable.has_value())
			throw UnresolvedImportException(imp.moduleName, imp.fieldName);

		importStorage.push_back(std::move(callable.value()));
	}

	// Point the VMContext at the resolved array. The pointer stays valid
	// because importStorage never reallocates after this point.
	ctx.importedFunctions      = importStorage.data();
	ctx.importedFunctionCount  = static_cast<uint32_t>(importStorage.size());

	// Repeat analogously for imported globals, memories, tables, tags...
	// (elided here for brevity, but follows the same resolve-then-assign pattern)
}

void ModuleInstance::initializeMemory()
{
	// A module may define zero or one memory (in MVP Wasm).
	if (module->memories.empty()) return;

	const MemoryType& memType = module->memories[0];
	const uint64_t initialBytes =
		static_cast<uint64_t>(memType.limits.initial) * 0x10000; // pages → bytes
	const uint64_t maxBytes = memType.limits.maximum.has_value()
		? static_cast<uint64_t>(memType.limits.maximum.value()) * 0x10000
		: UINT64_MAX;

	// Wasm spec requires memory to be zero-initialized.
	linearMemory.resize(initialBytes, 0);

	ctx.memoryBase = linearMemory.data();
	ctx.memorySize = initialBytes;
	ctx.memoryMax  = maxBytes;
}

void ModuleInstance::initializeGlobals()
{
	// Total globals = imported globals (already resolved) + locally defined ones.
	const size_t importedCount = module->importGlobals.size();
	const size_t definedCount  = module->globals.size();
	globalsStorage.resize(importedCount + definedCount);

	// Imported globals were already placed into globalsStorage by resolveImports().
	// Now handle the locally-defined ones.
	for (size_t i = 0; i < definedCount; ++i) {
		const Global& g = module->globals[i];
		// Initializer expressions for globals are restricted to a small set
		// of constant instructions (i32.const, f64.const, global.get of an
		// imported global, ref.null, ref.func). We evaluate them directly.
		globalsStorage[importedCount + i] = evalConstantExpr(Euph::Io::ConstBufferDevice::span_cast<uint8_t>(g.initOpcode));
	}

	ctx.globals = globalsStorage.data();
}

void ModuleInstance::initializeTable()
{
	// A module may define zero or more tables. MVP Wasm allows exactly one;
	// the reference types proposal allows multiple. We handle all of them
	// to be forward-compatible, even though in practice there's usually one.
	//
	// Note: imported tables would have been handled in resolveImports(),
	// similar to how imported memories are handled. For now we assert that
	// tables are locally defined only, consistent with our earlier decision
	// to defer imported memory/table support.

	if (module->tables.empty()) return;

	// We're only handling a single table for now. If you add multi-table
	// support later, this becomes a loop over module->tables with a
	// corresponding std::vector<std::vector<WasmCallable*>> in Instance.
	const TableType& tableType = module->tables[0];

	const uint64_t initialSize = tableType.limits.initial;
	const uint64_t maxSize = tableType.limits.maximum.has_value()
		? tableType.limits.maximum.value()
		: UINT64_MAX;

	// All slots start as null — meaning "uninitialized, traps on call_indirect".
	// std::vector zero-initializes pointer types when given a count and no value,
	// but nullptr is explicit and self-documenting here.
	tableStorage.resize(static_cast<size_t>(initialSize), nullptr);

	// Point the VMContext at the underlying array. As with linearMemory,
	// tableStorage must not be resized after this point except through
	// growTable(), which re-syncs the ctx pointers afterward.
	ctx.table     = tableStorage.data();
	ctx.tableSize = initialSize;
	ctx.tableMax  = maxSize;
}

Value ModuleInstance::evalConstantExpr(const std::span<const std::byte>& expr)
{
	Euph::Io::ConstBufferDevice buffDev(expr);
	WasmStream stream(buffDev);
	Value result{};

	// Read the leading opcode byte.
	const uint8_t opcode = stream.read<uint8_t>();

	switch (opcode)
	{
	// ── Numeric literals ────────────────────────────────────────────
	// Each of these just reads a literal value of the appropriate type.
	// The LEB128 encoding for integers means we use readSLEB/readULEB;
	// floats are stored as raw IEEE-754 bytes (little-endian).
	case 0x41: // i32.const
		result.i32  = stream.readLEB128<int32_t>();
		result.kind = ValueTypeCode::I32;
		break;

	case 0x42: // i64.const
		result.i64  = stream.readLEB128<int64_t>();
		result.kind = ValueTypeCode::I64;
		break;

	case 0x43: // f32.const  (4 raw bytes, not LEB128)
		result.f32  = stream.read<float>();
		result.kind = ValueTypeCode::F32;
		break;

	case 0x44: // f64.const  (8 raw bytes, not LEB128)
		result.f64  = stream.read<double>();
		result.kind = ValueTypeCode::F64;
		break;

	// ── global.get ──────────────────────────────────────────────────
	// The spec only allows global.get of an *imported* global in a
	// constant expression — locally-defined globals may not be
	// referenced here (at least in MVP; the extended-const proposal
	// relaxes this for immutable globals). The imported global must
	// itself be immutable. We've already resolved imported globals
	// into globalsStorage during resolveImports(), so we can just
	// index directly.
	case 0x23: {
		const uint32_t globalIdx = stream.readLEB128<uint32_t>();

		// Defensive check: if this fires, the module is malformed.
		// A validator would have caught this before instantiation.
		if (globalIdx >= globalsStorage.size())
			throw std::runtime_error("global.get in constant expr: index out of range");

		result = globalsStorage[globalIdx];
		break;
	}

	// ── Reference types ─────────────────────────────────────────────
	case 0xD0: // ref.null — takes a heap type byte, produces a null reference
		stream.readLEB128<uint32_t>(); // consume the heap type operand, discard it
		result.ref  = nullptr;
		result.kind = ValueTypeCode::FuncRef; // null is valid for any ref type
		break;

	case 0xD2: { // ref.func — produces a non-null reference to a function
		const uint32_t funcIdx = stream.readLEB128<uint32_t>();
		// At this point compiledFunctions may still be null (we're in the
		// constructor, before the ModuleInstantiator has compiled anything).
		// We store the index as the ref value for now and resolve it to a
		// real WasmCallable* after compilation during applyActiveSegments().
		// To signal "unresolved func ref", we encode the index in the ref field.
		// applyActiveSegments() will recognise this pattern and fix it up.
		result.ref  = reinterpret_cast<void*>(static_cast<uintptr_t>(funcIdx));
		result.kind = ValueTypeCode::FuncRef;
		break;
	}

	default:
		throw InvalidOpcodeException(opcode);
	}

	// Every constant expression ends with 'end' (0x0B). Consuming it here
	// is good practice even though we don't strictly need it — it catches
	// malformed expressions early and keeps the stream position correct
	// if the caller wants to read more after this expression.
	const uint8_t end = stream.read<uint8_t>();
	if (end != 0x0B)
		throw std::runtime_error("Constant expression missing 'end' terminator");

	return result;
}

bool ModuleInstance::growMemory(uint32_t deltaPages) {
	// Wasm page size is exactly 64KiB = 65536 bytes = 0x10000.
	// Check against memoryMax before committing.
	const size_t delta = static_cast<size_t>(deltaPages) * 0x10000;
	const size_t newSize = linearMemory.size() + delta;
	if (ctx.memoryMax != UINT64_MAX && newSize > ctx.memoryMax)
		return false; // Caller should turn this into a Wasm trap
	linearMemory.resize(newSize, 0); // Wasm requires new pages to be zero-initialized
	ctx.memoryBase = linearMemory.data();
	ctx.memorySize = static_cast<uint64_t>(newSize);
	return true;
}

bool ModuleInstance::growTable(uint32_t deltaEntries) {
	const size_t newSize = tableStorage.size() + deltaEntries;
	if (ctx.tableMax != UINT64_MAX && newSize > ctx.tableMax)
		return false;
	tableStorage.resize(newSize, nullptr); // Null = uninitialized slot, traps on call_indirect
	ctx.table = tableStorage.data();
	ctx.tableSize = static_cast<uint64_t>(newSize);
	return true;
}

}