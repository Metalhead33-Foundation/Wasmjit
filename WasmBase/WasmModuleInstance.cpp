#include "WasmModuleInstance.hpp"
#include "WasmException.hpp"
#include "WasmStore.hpp"
#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include <Elvavena/Io/ElvDataStream.hpp>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
namespace WASM {

std::unique_ptr<ModuleInstance> ModuleInstantiator::instantiate(const Module& module, ImportResolver& resolver)
{
	// The constructor does all the backend-agnostic setup.
	auto instance = std::make_unique<ModuleInstance>(module, resolver);

	// Two-pass compilation: first declare all functions (so mutual
	// recursion works), then compile all bodies.
	translateTypes(*instance, module, instance->internals);
	declareFunctions(*instance, module, instance->internals);
	compileFunctions(*instance, module, instance->internals);

	// Active element and data segments are applied after compilation
	// because element segments can reference functions by index, and
	// we need the compiled handles to be present first.
	applyActiveSegments(*instance, module, instance->internals);

	// Finally, call the start function if the module has one.
	if (module.hasStartFunction)
		callStartFunction(*instance, module.startFunctionIndex, instance->internals);

	return instance;
}

Value ModuleInstantiator::evalConstantExpr(const std::span<const std::byte>& expr, ModuleInstance& instance)
{
	return instance.evalConstantExpr(expr);
}

void ModuleInstantiator::applyActiveSegments(ModuleInstance& instance, const Module& module, ModuleInstanceInternals& internals)
{
	// ── Data Segments ───────────────────────────────────────────────────────
	// Each active data segment copies raw bytes into linear memory.
	// Passive segments (mode != 0) are skipped here — they wait for
	// memory.init instructions at runtime to activate them.
	for (const auto& seg : module.dataSegments)
	{
		// Mode 0 = active with implicit memory index 0.
		// Mode 2 = active with explicit memory index (multi-memory proposal).
		// Mode 1 = passive. We skip passive segments entirely here.
		if (seg.mode == 1) continue;

		// Evaluate the constant offset expression to get the destination
		// address within linear memory. This is almost always just an
		// i32.const followed by end, but we use the general evaluator
		// for correctness.
		const Value offsetVal = evalConstantExpr(
			Euph::Io::ConstBufferDevice::span_cast<uint8_t>(seg.offsetExpr), instance);
		const uint64_t offset = static_cast<uint64_t>(offsetVal.i32);

		// Active segments name their target memory explicitly (mode 0 implies
		// memory 0; mode 2 carries a memory index — multi-memory proposal).
		if (seg.memoryIdx >= internals.memoryRefs.size())
			throw std::runtime_error("active data segment references an unknown memory");
		LinearMemory* memory = internals.memoryRefs[seg.memoryIdx];

		// Wasm spec: trap if the segment would write past the end of memory.
		// This is a hard instantiation failure, not a soft error.
		if (offset + seg.data.size() > memory->memorySize)
			throw SegmentOutOfBoundsException(offset, seg.data.size(), memory->memorySize);

		// The actual copy. memoryBase is already a uint8_t*, so this is
		// just a pointer-offset write — no JIT involvement needed.
		std::memcpy(
			memory->memoryBase + offset,
			seg.data.data(),
			seg.data.size());
	}

	// ── Element Segments ────────────────────────────────────────────────────
	// Each active element segment writes function references into a table.
	// The structure mirrors data segments but operates on WasmCallable*
	// entries rather than raw bytes.
	const uint32_t importedFuncCount =
		static_cast<uint32_t>(module.importFunctions.size());

	for (const auto& seg : module.elementSegments)
	{
		// isActive() returns false for passive (mode 1) and declarative
		// (mode 3) segments. We only process active ones here.
		if (!seg.isActive()) continue;

		// Same constant-expression evaluation for the table offset.
		const Value offsetVal = evalConstantExpr(
			Euph::Io::ConstBufferDevice::span_cast<uint8_t>(seg.offsetExpr), instance);
		const uint64_t tableOffset = static_cast<uint64_t>(offsetVal.i32);

		// Determine how many entries this segment contributes.
		// A segment uses either initIndices (MVP index form) or
		// initExprs (reference-types constant-expression form), never both.
		const size_t entryCount = seg.initIndices.empty()
			? seg.initExprs.size()
			: seg.initIndices.size();

		if (tableOffset + entryCount > instance.ctx.tableSize)
			throw std::runtime_error("Element segment out of bounds");

		for (size_t i = 0; i < entryCount; ++i)
		{
			Callable* callable = nullptr;

			if (!seg.initIndices.empty())
			{
				// Simple MVP form: each entry is a raw function index.
				const uint32_t funcIdx = seg.initIndices[i];

				if (funcIdx < importedFuncCount)
				{
					// This index refers to an imported function.
					// The WasmCallable was placed into importStorage
					// by resolveImports(), so we can take its address.
					callable = &instance.internals.importStorage[funcIdx];
				}
				else
				{
					// This index refers to an internally compiled function.
					// compiledFunctions[adjusted] is a void* to the compiled
					// code; we need a WasmCallable wrapper for table storage.
					// We create these wrappers during declareFunctions() and
					// store them in internalCallables[], which is parallel to
					// compiledFunctions[]. See note below.
					const uint32_t internalIdx = funcIdx - importedFuncCount;
					callable = &instance.internals.internalCallables[internalIdx];
				}
			}
			else
			{
				// Reference-types form: each entry is a constant expression
				// that evaluates to a funcref or externref. Evaluate it and
				// retrieve the resulting callable.
				const Value refVal = evalConstantExpr(
					Euph::Io::ConstBufferDevice::span_cast<uint8_t>(seg.initExprs[i]), instance);
				if (refVal.ref == nullptr) {
					callable = nullptr;
				} else {
					const uintptr_t raw = reinterpret_cast<uintptr_t>(refVal.ref);
					const uint32_t totalFuncs = static_cast<uint32_t>(
						module.importFunctions.size() + module.internalFunctionTypeIndices.size());
					if (refVal.kind == ValueTypeCode::FuncRef && raw < totalFuncs) {
						if (raw < importedFuncCount)
							callable = &instance.internals.importStorage[raw];
						else
							callable = &instance.internals.internalCallables[raw - importedFuncCount];
					} else {
						callable = static_cast<Callable*>(refVal.ref);
					}
				}
			}

			// Write the callable pointer into the table at the right slot.
			instance.internals.tableStorage[tableOffset + i] = callable;
		}

		// Keep ctx.table in sync — tableStorage.data() doesn't change here
		// since we're not resizing, but being explicit makes the invariant clear.
		instance.ctx.table = instance.internals.tableStorage.data();
	}
}
ModuleInstance::ModuleInstance(const Module& module, ImportResolver& resolver)
	: module(&module)
{
	ctx = {};
	ctx.module = &module;

	// ── Step 1: Resolve and store all imports ───────────────────────
	// Imports must come first because they occupy the low indices in
	// each index space. We need importedFunctions populated before we
	// can evaluate any initializer expressions (which might call imports).
	resolveImports(resolver);

	// ── Step 2: Set up the memory index space ──────────────────────
	// Imported memories were resolved by resolveImports(); locally-defined
	// ones are allocated in the Store here. Either way the instance only
	// borrows the resulting LinearMemory*.
	initializeMemories();

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
	internals.compiledFunctions.resize(module.internalFunctionTypeIndices.size(), nullptr);
	internals.dataSegmentDropped.resize(module.dataSegments.size(), false);
	internals.elementSegmentDropped.resize(module.elementSegments.size(), false);
}

std::optional<Callable> ModuleInstance::exportedFunction(std::string_view name) const
{
	const uint32_t importedFuncCount = static_cast<uint32_t>(module->importFunctions.size());

	for (const Export& ex : module->exports) {
		if (ex.kind != ExternalKind::Function || ex.name != name)
			continue;

		if (ex.index < importedFuncCount) {
			if (ex.index >= internals.importStorage.size())
				std::abort();
			return internals.importStorage[ex.index];
		}

		const uint32_t internalIdx = ex.index - importedFuncCount;
		if (internalIdx >= internals.internalCallables.size())
			std::abort();
		return internals.internalCallables[internalIdx];
	}

	return std::nullopt;
}

void ModuleInstance::registerExports(ImportRegistrar& registrar, std::string_view moduleName) const
{
	const uint32_t importedFuncCount = static_cast<uint32_t>(module->importFunctions.size());

	for (const Export& ex : module->exports) {
		switch (ex.kind) {
		case ExternalKind::Function: {
			Callable callable{};
			if (ex.index < importedFuncCount) {
				if (ex.index >= internals.importStorage.size())
					std::abort();
				callable = internals.importStorage[ex.index];
			} else {
				const uint32_t internalIdx = ex.index - importedFuncCount;
				if (internalIdx >= internals.internalCallables.size())
					std::abort();
				callable = internals.internalCallables[internalIdx];
			}
			registrar.registerFunction(moduleName, ex.name, callable);
			break;
		}
		case ExternalKind::Global: {
			if (ctx.globals == nullptr)
				std::abort();
			registrar.registerGlobal(moduleName, ex.name, ctx.globals[ex.index]);
			break;
		}
		case ExternalKind::Memory: {
			// Publish the borrowed view so that other modules can import the
			// very same linear memory (the "shared memory" case).
			if (ex.index >= internals.memoryRefs.size())
				std::abort();
			registrar.registerMemory(moduleName, ex.name, internals.memoryRefs[ex.index]);
			break;
		}
		case ExternalKind::Table: {
			if (ex.index != 0)
				std::abort();
			registrar.registerTable(moduleName, ex.name,
								ImportedTable{ctx.table, ctx.tableSize, ctx.tableMax});
			break;
		}
		case ExternalKind::Tag:
			registrar.registerTag(moduleName, ex.name, ex.index);
			break;
		}
	}
}

void ModuleInstance::resolveImports(ImportResolver& resolver)
{
	// Pre-allocate so we can index by import order without reallocation.
	internals.importStorage.reserve(module->importFunctions.size());

	for (const auto& imp : module->importFunctions) {
		auto callable = resolver.resolveFunction(
			imp.moduleName, imp.fieldName, imp.typeIdx);

		// A missing import is a hard instantiation-time error in Wasm,
		// not a runtime trap. We fail loudly here rather than storing
		// a null that would cause a mysterious crash later.
		if (!callable.has_value())
			throw UnresolvedImportException(imp.moduleName, imp.fieldName);

		internals.importStorage.push_back(std::move(callable.value()));
	}

	// Point the VMContext at the resolved array. The pointer stays valid
	// because importStorage never reallocates after this point.
	ctx.importedFunctions      = internals.importStorage.data();
	ctx.importedFunctionCount  = static_cast<uint32_t>(internals.importStorage.size());

	// ── Memory imports ──────────────────────────────────────────────
	// Resolve imported memories first: they occupy the low indices of the
	// memory index space. The result is a borrowed LinearMemory* owned by
	// the Store — resolving the same import from two modules yields the
	// same pointer, which is how two modules share one linear memory.
	internals.memoryRefs.reserve(module->importMemories.size() + module->memories.size());
	for (const auto& imp : module->importMemories) {
		std::optional<LinearMemory*> memory =
			resolver.resolveMemory(imp.moduleName, imp.fieldName, imp.memory);
		if (!memory.has_value())
			throw UnresolvedImportException(imp.moduleName, imp.fieldName);
		internals.memoryRefs.push_back(memory.value());
	}

	// Imported globals, tables and tags follow the same resolve-then-assign
	// pattern; they are handled elsewhere (globals/tables) or not yet wired.
}

void ModuleInstance::initializeMemories()
{
	// A module may define zero or more memories (MVP allows one; the
	// multi-memory proposal allows several). Imported memories were already
	// appended to memoryRefs by resolveImports(), in import order; the
	// locally-defined ones follow here.
	Store& store = Store::global();

	for (const MemoryType& memType : module->memories) {
		const bool isShared = (memType.limits.flags & 0x02) != 0; // threads proposal
		const uint64_t initialPages = memType.limits.initial;
		const uint64_t maxPages = memType.limits.maximum.has_value()
			? memType.limits.maximum.value()
			: UINT64_MAX;

		const Store::MemoryId id = store.createLinearMemory(initialPages, maxPages, isShared);
		LinearMemory* memory = store.memory(id);
		if (memory == nullptr)
			throw std::runtime_error("failed to allocate linear memory");
		// Wasm requires new memories to be zero-initialized; both store-backed
		// memory kinds guarantee this at allocation.
		internals.memoryRefs.push_back(memory);
	}

	// The VMContext points straight at the borrowed index space. Because the
	// Store keeps each memory at a stable address and memoryRefs is final at
	// this point, ctx.memories stays valid for the lifetime of the instance.
	ctx.memories    = internals.memoryRefs.data();
	ctx.memoryCount = static_cast<uint32_t>(internals.memoryRefs.size());
}

void ModuleInstance::initializeGlobals()
{
	// Total globals = imported globals (already resolved) + locally defined ones.
	const size_t importedCount = module->importGlobals.size();
	const size_t definedCount  = module->globals.size();
	internals.globalsStorage.resize(importedCount + definedCount);

	// Imported globals were already placed into globalsStorage by resolveImports().
	// Now handle the locally-defined ones.
	for (size_t i = 0; i < definedCount; ++i) {
		const Global& g = module->globals[i];
		// Initializer expressions for globals are restricted to a small set
		// of constant instructions (i32.const, f64.const, global.get of an
		// imported global, ref.null, ref.func). We evaluate them directly.
		internals.globalsStorage[importedCount + i] = evalConstantExpr(Euph::Io::ConstBufferDevice::span_cast<uint8_t>(g.initOpcode));
	}

	ctx.globals = internals.globalsStorage.data();
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
	internals.tableStorage.resize(static_cast<size_t>(initialSize), nullptr);

	// Point the VMContext at the underlying array. tableStorage must not be
	// resized after this point except through growTable(), which re-syncs the
	// ctx pointers afterward.
	ctx.table     = internals.tableStorage.data();
	ctx.tableSize = initialSize;
	ctx.tableMax  = maxSize;
}

Value ModuleInstance::evalConstantExpr(const std::span<const std::byte>& expr)
{
	Euph::Io::ConstBufferDevice buffDev(expr);
	DWasmStream stream(buffDev);
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
		if (globalIdx >= internals.globalsStorage.size())
			throw std::runtime_error("global.get in constant expr: index out of range");

		result = internals.globalsStorage[globalIdx];
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

int32_t ModuleInstance::growMemory(uint32_t memIdx, uint32_t deltaPages) {
	if (memIdx >= internals.memoryRefs.size())
		return -1;

	LinearMemory* memory = internals.memoryRefs[memIdx];
	const uint32_t oldPages = static_cast<uint32_t>(memory->memorySize / 0x10000);

	// The Store owns the memory (and therefore its growth policy/limits).
	if (!Store::growMemory(memory, deltaPages))
		return -1; // Caller turns this into a Wasm trap

	return static_cast<int32_t>(oldPages);
}

bool ModuleInstance::growTable(uint32_t deltaEntries) {
	const size_t newSize = internals.tableStorage.size() + deltaEntries;
	if (ctx.tableMax != UINT64_MAX && newSize > ctx.tableMax)
		return false;
	internals.tableStorage.resize(newSize, nullptr); // Null = uninitialized slot, traps on call_indirect
	ctx.table = internals.tableStorage.data();
	ctx.tableSize = static_cast<uint64_t>(newSize);
	return true;
}

void ModuleInstance::memoryInit(uint32_t memIdx, uint32_t dataIdx, uint32_t dstOffset, uint32_t srcOffset, uint32_t len)
{
	if (memIdx >= internals.memoryRefs.size())
		std::abort();
	if (dataIdx >= module->dataSegments.size())
		std::abort();
	if (internals.dataSegmentDropped[dataIdx])
		std::abort();

	LinearMemory* memory = internals.memoryRefs[memIdx];
	const DataSegment& seg = module->dataSegments[dataIdx];
	if (srcOffset > seg.data.size() || len > seg.data.size() - srcOffset)
		std::abort();
	if (static_cast<uint64_t>(dstOffset) + len > memory->memorySize)
		std::abort();

	std::memcpy(memory->memoryBase + dstOffset, seg.data.data() + srcOffset, len);
}

void ModuleInstance::dataDrop(uint32_t dataIdx)
{
	if (dataIdx >= internals.dataSegmentDropped.size())
		std::abort();
	internals.dataSegmentDropped[dataIdx] = true;
}

void ModuleInstance::tableInit(uint32_t elemIdx, uint32_t dstOffset, uint32_t srcOffset, uint32_t len)
{
	if (elemIdx >= module->elementSegments.size())
		std::abort();
	if (internals.elementSegmentDropped[elemIdx])
		std::abort();

	const ElementSegment& seg = module->elementSegments[elemIdx];
	const size_t entryCount = seg.initIndices.empty() ? seg.initExprs.size() : seg.initIndices.size();
	if (srcOffset > entryCount || len > entryCount - srcOffset)
		std::abort();
	if (static_cast<uint64_t>(dstOffset) + len > ctx.tableSize)
		std::abort();

	const uint32_t importedFuncCount =
		static_cast<uint32_t>(module->importFunctions.size());
	const uint32_t totalFuncs = static_cast<uint32_t>(
		module->importFunctions.size() + module->internalFunctionTypeIndices.size());

	for (uint32_t i = 0; i < len; ++i) {
		Callable* callable = nullptr;
		const size_t entryIndex = srcOffset + i;
		if (!seg.initIndices.empty()) {
			const uint32_t funcIdx = seg.initIndices[entryIndex];
			if (funcIdx < importedFuncCount)
				callable = &internals.importStorage[funcIdx];
			else
				callable = &internals.internalCallables[funcIdx - importedFuncCount];
		} else {
			const Value refVal = evalConstantExpr(
				Euph::Io::ConstBufferDevice::span_cast<uint8_t>(seg.initExprs[entryIndex]));
			if (refVal.ref == nullptr) {
				callable = nullptr;
			} else {
				const uintptr_t raw = reinterpret_cast<uintptr_t>(refVal.ref);
				if (refVal.kind == ValueTypeCode::FuncRef && raw < totalFuncs) {
					if (raw < importedFuncCount)
						callable = &internals.importStorage[raw];
					else
						callable = &internals.internalCallables[raw - importedFuncCount];
				} else {
					callable = static_cast<Callable*>(refVal.ref);
				}
			}
		}
		internals.tableStorage[dstOffset + i] = callable;
	}

	ctx.table = internals.tableStorage.data();
}

void ModuleInstance::elemDrop(uint32_t elemIdx)
{
	if (elemIdx >= internals.elementSegmentDropped.size())
		std::abort();
	internals.elementSegmentDropped[elemIdx] = true;
}

void ModuleInstance::bufferInitFromData(uint32_t dataIdx, void* dst, uint32_t srcOffset, uint32_t lenBytes)
{
	if (dataIdx >= module->dataSegments.size())
		std::abort();
	if (internals.dataSegmentDropped[dataIdx])
		std::abort();

	const DataSegment& seg = module->dataSegments[dataIdx];
	if (srcOffset > seg.data.size() || lenBytes > seg.data.size() - srcOffset)
		std::abort();
	std::memcpy(dst, seg.data.data() + srcOffset, lenBytes);
}

void ModuleInstance::bufferInitFromElems(uint32_t elemIdx, void* dst, uint32_t srcOffset, uint32_t lenElems)
{
	if (elemIdx >= module->elementSegments.size())
		std::abort();
	if (internals.elementSegmentDropped[elemIdx])
		std::abort();

	const ElementSegment& seg = module->elementSegments[elemIdx];
	const size_t entryCount = seg.initIndices.empty() ? seg.initExprs.size() : seg.initIndices.size();
	if (srcOffset > entryCount || lenElems > entryCount - srcOffset)
		std::abort();

	Callable** out = static_cast<Callable**>(dst);
	const uint32_t importedFuncCount =
		static_cast<uint32_t>(module->importFunctions.size());
	const uint32_t totalFuncs = static_cast<uint32_t>(
		module->importFunctions.size() + module->internalFunctionTypeIndices.size());

	for (uint32_t i = 0; i < lenElems; ++i) {
		Callable* callable = nullptr;
		const size_t entryIndex = srcOffset + i;
		if (!seg.initIndices.empty()) {
			const uint32_t funcIdx = seg.initIndices[entryIndex];
			if (funcIdx < importedFuncCount)
				callable = &internals.importStorage[funcIdx];
			else
				callable = &internals.internalCallables[funcIdx - importedFuncCount];
		} else {
			const Value refVal = evalConstantExpr(
				Euph::Io::ConstBufferDevice::span_cast<uint8_t>(seg.initExprs[entryIndex]));
			if (refVal.ref == nullptr) {
				callable = nullptr;
			} else {
				const uintptr_t raw = reinterpret_cast<uintptr_t>(refVal.ref);
				if (refVal.kind == ValueTypeCode::FuncRef && raw < totalFuncs) {
					if (raw < importedFuncCount)
						callable = &internals.importStorage[raw];
					else
						callable = &internals.internalCallables[raw - importedFuncCount];
				} else {
					callable = static_cast<Callable*>(refVal.ref);
				}
			}
		}
		out[i] = callable;
	}
}

void* ModuleInstance::allocateStructObject(uint32_t size, uint32_t typeIndex)
{
	uint8_t* object = static_cast<uint8_t*>(std::calloc(1, size));
	if (!object)
		std::abort();
	*reinterpret_cast<uint32_t*>(object) = typeIndex;
	internals.gcObjectTypes[object] = typeIndex;
	return object;
}

void* ModuleInstance::allocateArrayObject(uint32_t headerSize, uint32_t elementSize, uint32_t length, uint32_t typeIndex)
{
	uint8_t* header = static_cast<uint8_t*>(std::calloc(1, headerSize));
	if (!header)
		std::abort();

	uint8_t* data = nullptr;
	if (length != 0) {
		data = static_cast<uint8_t*>(std::calloc(length, elementSize));
		if (!data)
			std::abort();
	}

	*reinterpret_cast<uint32_t*>(header) = typeIndex;
	*reinterpret_cast<uint32_t*>(header + sizeof(uint32_t)) = length;
	*reinterpret_cast<void**>(header + sizeof(uint32_t) * 2) = data;
	internals.gcObjectTypes[header] = typeIndex;
	return header;
}

bool ModuleInstance::tryGetGcTypeIndex(const void* ref, uint32_t& typeIndex) const
{
	auto it = internals.gcObjectTypes.find(ref);
	if (it == internals.gcObjectTypes.end())
		return false;
	typeIndex = it->second;
	return true;
}

bool ModuleInstance::refMatchesHeapType(const void* ref, const HeapType& heapType, bool nullable) const
{
	if (ref == nullptr)
		return nullable;

	const uintptr_t raw = reinterpret_cast<uintptr_t>(ref);
	if ((raw & 1u) != 0) {
		if (heapType.isTypeIndex)
			return false;
		switch (heapType.abstract) {
		case AbstractHeapType::Any:
		case AbstractHeapType::Eq:
		case AbstractHeapType::I31:
			return true;
		default:
			return false;
		}
	}

	for (const Callable& callable : internals.importStorage) {
		if (&callable != ref)
			continue;
		if (heapType.isTypeIndex)
			return module->isSubtype(callable.typeIndex, heapType.typeIndex);
		switch (heapType.abstract) {
		case AbstractHeapType::Any:
		case AbstractHeapType::Func:
			return true;
		case AbstractHeapType::NoFunc:
		case AbstractHeapType::None:
			return false;
		default:
			return false;
		}
	}

	for (const Callable& callable : internals.internalCallables) {
		if (&callable != ref)
			continue;
		if (heapType.isTypeIndex)
			return module->isSubtype(callable.typeIndex, heapType.typeIndex);
		switch (heapType.abstract) {
		case AbstractHeapType::Any:
		case AbstractHeapType::Func:
			return true;
		case AbstractHeapType::NoFunc:
		case AbstractHeapType::None:
			return false;
		default:
			return false;
		}
	}

	uint32_t gcTypeIndex = 0;
	if (tryGetGcTypeIndex(ref, gcTypeIndex)) {
		if (heapType.isTypeIndex)
			return module->isSubtype(gcTypeIndex, heapType.typeIndex);

		const Subtype& subtype = module->types[gcTypeIndex];
		switch (heapType.abstract) {
		case AbstractHeapType::Any:
		case AbstractHeapType::Eq:
			return true;
		case AbstractHeapType::Struct:
			return subtype.isStruct();
		case AbstractHeapType::Array:
			return subtype.isArray();
		case AbstractHeapType::None:
		case AbstractHeapType::NoExtern:
		case AbstractHeapType::NoFunc:
		case AbstractHeapType::Func:
		case AbstractHeapType::Extern:
		case AbstractHeapType::I31:
			return false;
		}
	}

	if (heapType.isTypeIndex)
		return false;

	switch (heapType.abstract) {
	case AbstractHeapType::Any:
	case AbstractHeapType::Extern:
	case AbstractHeapType::NoFunc:
		return true;
	default:
		return false;
	}
}

}
