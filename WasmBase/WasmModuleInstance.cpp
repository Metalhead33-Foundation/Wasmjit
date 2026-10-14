#include "WasmModuleInstance.hpp"
#include "WasmException.hpp"
#include "WasmOpcode.hpp"
#include "WasmStore.hpp"
#include "WasmTrap.hpp"
#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include <Elvavena/Io/ElvDataStream.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <stdexcept>
namespace WASM {

TailCallState g_tailCallState;

namespace {
// `ref.func $f` is stored in a constant-expression Value as a *biased* function
// index (index + 1), so that function index 0 is distinguishable from the null
// pointer that `ref.null` produces. Both element segments and global
// initializers decode it the same way.
inline uintptr_t encodeRefFuncIndex(uint32_t funcIdx)
{
	return static_cast<uintptr_t>(funcIdx) + 1u;
}
inline uint32_t decodeRefFuncIndex(uintptr_t raw)
{
	return static_cast<uint32_t>(raw - 1u);
}
} // namespace

std::unique_ptr<ModuleInstance> ModuleInstantiator::instantiate(const Module& module, ImportResolver& resolver)
{
	// The constructor does all the backend-agnostic setup.
	auto instance = std::make_unique<ModuleInstance>(module, resolver);

	// Two-pass compilation: first declare all functions (so mutual
	// recursion works), then compile all bodies.
	translateTypes(*instance, module, instance->internals);
	declareFunctions(*instance, module, instance->internals);
	compileFunctions(*instance, module, instance->internals);

	// `ref.func` globals could only be stored as sentinels during construction;
	// resolve them now that the callable tables exist.
	instance->resolveGlobalRefFuncs();

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

Callable* ModuleInstance::callableFromSegmentRef(const Value& ref)
{
	if (ref.ref == nullptr)
		return nullptr;

	const uint32_t importedFuncCount = static_cast<uint32_t>(module->importFunctions.size());
	const uint32_t totalFuncs = static_cast<uint32_t>(
		module->importFunctions.size() + module->internalFunctionTypeIndices.size());
	const uint32_t funcIdx = decodeRefFuncIndex(reinterpret_cast<uintptr_t>(ref.ref));
	if (ref.kind == ValueTypeCode::FuncRef && funcIdx < totalFuncs) {
		if (funcIdx < importedFuncCount)
			return &internals.importStorage[funcIdx];
		return &internals.internalCallables[funcIdx - importedFuncCount];
	}
	// A GC reference (or a host externref) is stored opaquely.
	return static_cast<Callable*>(ref.ref);
}

void ModuleInstantiator::applyActiveSegments(ModuleInstance& instance, const Module& module, ModuleInstanceInternals& internals)
{
	// ── Data Segments ───────────────────────────────────────────────────────
	// Each active data segment copies raw bytes into linear memory. Active and
	// declarative segments are *dropped* once instantiation is done: the spec
	// makes them behave as empty for a later memory.init/data.drop, which is
	// what elem.wast's "implicitly dropped elements" cases check.
	for (size_t dataIdx = 0; dataIdx < module.dataSegments.size(); ++dataIdx)
	{
		const DataSegment& seg = module.dataSegments[dataIdx];
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

		// An applied active segment is dropped (see the loop comment above).
		internals.dataSegmentDropped[dataIdx] = true;
	}

	// ── Element segment references ─────────────────────────────────────────
	// An expression-form segment holds *references*: evaluate each entry once
	// here, so a later table.init / array.new_elem copies those same
	// references instead of re-evaluating the expression (docs/proposals/
	// GC.md G4; BULK_MEMORY.md §4). Index-form segments need no cache.
	for (size_t elemIdx = 0; elemIdx < module.elementSegments.size(); ++elemIdx) {
		const ElementSegment& seg = module.elementSegments[elemIdx];
		std::vector<Value>& refs = instance.internals.elementSegmentRefs[elemIdx];
		refs.clear();
		if (!seg.initIndices.empty() || seg.isDeclarative())
			continue;
		refs.reserve(seg.initExprs.size());
		for (const std::vector<uint8_t>& expr : seg.initExprs)
			refs.push_back(evalConstantExpr(
				Euph::Io::ConstBufferDevice::span_cast<uint8_t>(expr), instance));
	}

	// ── Element Segments ────────────────────────────────────────────────────
	// Each active element segment writes function references into a table.
	// The structure mirrors data segments but operates on WasmCallable*
	// entries rather than raw bytes. Declarative segments are never placed
	// anywhere and active ones are dropped afterwards, so both become empty.
	const uint32_t importedFuncCount =
		static_cast<uint32_t>(module.importFunctions.size());

	for (size_t elemIdx = 0; elemIdx < module.elementSegments.size(); ++elemIdx)
	{
		const ElementSegment& seg = module.elementSegments[elemIdx];
		// isActive() returns false for passive (mode 1) and declarative
		// (mode 3) segments. We only process active ones here.
		if (!seg.isActive()) {
			if (seg.isDeclarative())
				internals.elementSegmentDropped[elemIdx] = true;
			continue;
		}

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

		if (static_cast<size_t>(seg.tableIdx) >= instance.internals.tableRefs.size())
			throw std::runtime_error("active element segment references an unknown table");
		TableInstance* table = instance.internals.tableRefs[seg.tableIdx];
		if (tableOffset + entryCount > table->size)
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
				// Expression form: use the reference computed once at
				// instantiation (never re-evaluate the expression here).
				callable = instance.callableFromSegmentRef(
					instance.internals.elementSegmentRefs[elemIdx][i]);
			}

			// Write the callable pointer into the store-owned table slot.
			table->base[tableOffset + i] = callable;
		}

		// An applied active segment is dropped (see the loop comment above).
		instance.internals.elementSegmentDropped[elemIdx] = true;
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
	internals.elementSegmentRefs.resize(module.elementSegments.size());
}

std::optional<Callable> ModuleInstance::exportedFunction(std::string_view name) const
{
	const uint32_t importedFuncCount = static_cast<uint32_t>(module->importFunctions.size());

	for (const Export& ex : module->exports) {
		if (ex.kind != ExternalKind::Function || ex.name != name)
			continue;

		if (ex.index < importedFuncCount) {
			if (ex.index >= internals.importStorage.size())
				throw std::runtime_error("module exports a function index outside its index space");
			return internals.importStorage[ex.index];
		}

		const uint32_t internalIdx = ex.index - importedFuncCount;
		if (internalIdx >= internals.internalCallables.size())
			throw std::runtime_error("module exports a function index outside its index space");
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
					throw std::runtime_error("module exports a function index outside its index space");
				callable = internals.importStorage[ex.index];
			} else {
				const uint32_t internalIdx = ex.index - importedFuncCount;
				if (internalIdx >= internals.internalCallables.size())
					throw std::runtime_error("module exports a function index outside its index space");
				callable = internals.internalCallables[internalIdx];
			}
			registrar.registerFunction(moduleName, ex.name, callable);
			break;
		}
		case ExternalKind::Global: {
			if (ctx.globals == nullptr)
				throw std::runtime_error("module exports a global before its globals were initialised");
			registrar.registerGlobal(moduleName, ex.name, ctx.globals[ex.index]);
			break;
		}
		case ExternalKind::Memory: {
			// Publish the borrowed view so that other modules can import the
			// very same linear memory (the "shared memory" case).
			if (ex.index >= internals.memoryRefs.size())
				throw std::runtime_error("module exports a memory index outside its index space");
			registrar.registerMemory(moduleName, ex.name, internals.memoryRefs[ex.index]);
			break;
		}
		case ExternalKind::Table: {
			// The table index space includes imported tables, so a valid export
			// index has a view; be defensive anyway. Registering the borrowed
			// view is how another module imports (and shares) this exact table.
			TableInstance* table = ex.index < internals.tableRefs.size()
				? internals.tableRefs[ex.index]
				: nullptr;
			registrar.registerTable(moduleName, ex.name,
								ImportedTable{table});
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
		// Cross-module type matching: give the resolver the canonical identity of
		// the importing module's declared function type.
		const TypeId expectedType = module->typeId(LocalTypeIdx{imp.typeIdx});
		auto callable = resolver.resolveFunction(imp.moduleName, imp.fieldName, expectedType);

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

	// ── Table imports ───────────────────────────────────────────────
	// Imported tables occupy the low indices of the table index space;
	// the module's own tables are appended by initializeTable(). Resolving
	// the same (module, field) from two modules yields the same borrowed
	// TableInstance*, i.e. library semantics require one Store-owned table.
	internals.tableRefs.reserve(module->importTables.size() + module->tables.size());
	for (const auto& imp : module->importTables) {
		std::optional<ImportedTable> table =
			resolver.resolveTable(imp.moduleName, imp.fieldName, imp.table);
		if (!table.has_value())
			throw UnresolvedImportException(imp.moduleName, imp.fieldName);
		if (table->table == nullptr)
			throw UnresolvedImportException(imp.moduleName, imp.fieldName);
		internals.tableRefs.push_back(table->table);
	}

	// ── Global imports ──────────────────────────────────────────────
	// Imported globals occupy the low indices of the global index space and
	// must be in place *before* any local initialiser evaluates a `global.get`
	// (docs/proposals/MUTABLE_GLOBALS.md MG-1, docs/proposals/GC.md G6).
	// initializeGlobals() later resizes this vector, which preserves these
	// entries, and `ctx.globals` then points at the combined array.
	internals.globalsStorage.resize(module->importGlobals.size());
	for (size_t i = 0; i < module->importGlobals.size(); ++i) {
		const ImportGlobal& imp = module->importGlobals[i];
		std::optional<Value> value =
			resolver.resolveGlobal(imp.moduleName, imp.fieldName, imp.global);
		if (!value.has_value())
			throw UnresolvedImportException(imp.moduleName, imp.fieldName);
		internals.globalsStorage[i] = value.value();
	}

	// Tags follow the same resolve-then-assign pattern but are not wired yet.
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
		uint64_t maxPages = memType.limits.maximum.has_value()
			? memType.limits.maximum.value()
			: UINT64_MAX;
		// A 32-bit memory's index space caps it at 65536 pages (4 GiB); a 64-bit
		// memory is bounded only by the host allocation budget (WasmStore).
		if (memType.indexType == MemoryIndexType::I32)
			maxPages = std::min<uint64_t>(maxPages, 65536);

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

void ModuleInstance::resolveGlobalRefFuncs()
{
	// evalConstantExpr() returns `ref.func` as a sentinel: the raw function
	// index encoded as a pointer. That is unavoidable because global
	// initializers run in the constructor, before any function handle exists.
	// Now that the callable tables are populated, rewrite those sentinels.
	const uint32_t importedFuncCount =
		static_cast<uint32_t>(module->importFunctions.size());
	const uint32_t totalFuncs = importedFuncCount +
		static_cast<uint32_t>(module->internalFunctionTypeIndices.size());

	for (Value& v : internals.globalsStorage) {
		if (v.kind != ValueTypeCode::FuncRef || v.ref == nullptr)
			continue;
		const uint32_t funcIdx = decodeRefFuncIndex(reinterpret_cast<uintptr_t>(v.ref));
		if (funcIdx >= totalFuncs)
			continue; // already a real Callable* (e.g. via an imported global)
		if (funcIdx < importedFuncCount)
			v.ref = &internals.importStorage[funcIdx];
		else
			v.ref = &internals.internalCallables[funcIdx - importedFuncCount];
	}
}

void ModuleInstance::initializeTable()
{
	// A module may define zero or more tables. MVP Wasm allows exactly one;
	// the reference types proposal allows multiple. We handle all of them
	// to be forward-compatible, even though in practice there's usually one.
	//
	// Imported tables occupy the low indices of the table index space; they
	// were already appended to tableRefs by resolveImports(). The module's own
	// tables follow here, so ctx.tables ends up pointing at the full space.
	internals.tableRefs.reserve(module->importTables.size() + module->tables.size());
	for (const TableType& tableType : module->tables) {
		const uint64_t initialSize = tableType.limits.initial;
		uint64_t maxSize = tableType.limits.maximum.has_value()
			? tableType.limits.maximum.value()
			: UINT64_MAX;
		// A 32-bit table's index space is 2^32-1 entries; a 64-bit table
		// (limits flag bit 2) is bounded only by the host allocation budget.
		if ((tableType.limits.flags & 0x04) == 0)
			maxSize = std::min<uint64_t>(maxSize, 0xFFFF'FFFFull);

		// Slots start null — "uninitialized, traps on call_indirect".
		const Store::TableId id = Store::global().createTable(initialSize, maxSize);
		TableInstance* view = Store::global().table(id);
		if (view == nullptr)
			throw std::runtime_error("failed to allocate table");
		internals.tableRefs.push_back(view);
	}

	ctx.tables     = internals.tableRefs.empty() ? nullptr : internals.tableRefs.data();
	ctx.tableCount = static_cast<uint32_t>(internals.tableRefs.size());
}

namespace {

// Runtime layout of GC aggregates must match LibJitTypeTranslator:
//   struct: [u32 TypeId][u32 refcount], then naturally-aligned fields;
//   array:  [u32 TypeId][u32 refcount][u32 length][void* data].
// The shared constants live in WasmGcHeader.hpp; keep them in sync with
// translateStruct / translateArray (docs/proposals/GC.md §6.3 / H1).
constexpr size_t kGcHeaderBytes = GcHeader::kHeaderBytes;
constexpr size_t kArrayLengthOffset = GcHeader::kArrayLengthOffset;
constexpr size_t kArrayDataOffset = GcHeader::kArrayDataOffset;

size_t storageBytes(const StorageType& st)
{
	if (st.isPacked)
		return st.val.opcode == ValueTypeCode::I8 ? 1 : 2;
	switch (st.val.opcode) {
	case ValueTypeCode::I32:
	case ValueTypeCode::F32: return 4;
	case ValueTypeCode::I64:
	case ValueTypeCode::F64: return 8;
	case ValueTypeCode::V128: return 16;
	default: return sizeof(void*); // references
	}
}

size_t storageAlign(const StorageType& st)
{
	const size_t bytes = storageBytes(st);
	return bytes == 16 ? 4 : bytes; // v128 lowers to a 4-aligned struct of ints
}

size_t alignUp(size_t value, size_t alignment) { return (value + alignment - 1) / alignment * alignment; }

std::vector<size_t> structFieldOffsets(const StructType& st)
{
	std::vector<size_t> offsets;
	offsets.reserve(st.fields.size());
	size_t offset = kGcHeaderBytes;
	for (const FieldType& f : st.fields) {
		offset = alignUp(offset, storageAlign(f.storageType));
		offsets.push_back(offset);
		offset += storageBytes(f.storageType);
	}
	return offsets;
}

size_t structSize(const StructType& st)
{
	size_t end = kGcHeaderBytes;
	size_t maxAlign = kGcHeaderBytes;
	for (const FieldType& f : st.fields) {
		const size_t a = storageAlign(f.storageType);
		end = alignUp(end, a) + storageBytes(f.storageType);
		if (a > maxAlign) maxAlign = a;
	}
	return alignUp(end, maxAlign);
}

size_t arraySize() { return alignUp(kArrayDataOffset + sizeof(void*), sizeof(void*)); }

void storeValue(void* base, size_t offset, const StorageType& st, const Value& v)
{
	uint8_t* p = static_cast<uint8_t*>(base) + offset;
	if (st.isPacked) {
		if (st.val.opcode == ValueTypeCode::I8) *reinterpret_cast<int8_t*>(p) = static_cast<int8_t>(v.i32);
		else *reinterpret_cast<int16_t*>(p) = static_cast<int16_t>(v.i32);
		return;
	}
	switch (st.val.opcode) {
	case ValueTypeCode::I32: *reinterpret_cast<int32_t*>(p) = v.i32; break;
	case ValueTypeCode::I64: *reinterpret_cast<int64_t*>(p) = v.i64; break;
	case ValueTypeCode::F32: *reinterpret_cast<float*>(p) = v.f32; break;
	case ValueTypeCode::F64: *reinterpret_cast<double*>(p) = v.f64; break;
	case ValueTypeCode::V128: std::memcpy(p, v.v128, 16); break;
	default: *reinterpret_cast<void**>(p) = v.ref; break;
	}
}

} // namespace

Value ModuleInstance::evalConstantExpr(const std::span<const std::byte>& expr)
{
	Euph::Io::ConstBufferDevice buffDev(expr);
	DWasmStream stream(buffDev);
	// Constant expressions are evaluated as a small stack machine, because GC
	// constructors (ref.i31, struct.new*, array.new*) consume operands that are
	// themselves constant instructions.
	std::vector<Value> stack;
	const auto popValue = [&stack]() -> Value {
		if (stack.empty())
			throw std::runtime_error("constant expression stack underflow");
		const Value v = stack.back();
		stack.pop_back();
		return v;
	};

	for (;;) {
		const uint8_t opcode = stream.read<uint8_t>();
		if (opcode == 0x0B) // end
			break;

		switch (opcode) {
		case 0x41: { Value v{}; v.i32 = stream.readLEB128<int32_t>(); v.kind = ValueTypeCode::I32; stack.push_back(v); break; }
		case 0x42: { Value v{}; v.i64 = stream.readLEB128<int64_t>(); v.kind = ValueTypeCode::I64; stack.push_back(v); break; }
		case 0x43: { Value v{}; v.f32 = stream.read<float>();     v.kind = ValueTypeCode::F32; stack.push_back(v); break; }
		case 0x44: { Value v{}; v.f64 = stream.read<double>();    v.kind = ValueTypeCode::F64; stack.push_back(v); break; }

		// Extended constant expressions (docs/proposals/EXTENDED_CONST.md
		// EC-1): integer add/sub/mul are the only arithmetic a Wasm 3.0
		// constant expression may contain. Compute with wrap-around and keep
		// the operand kind as the result kind.
		case 0x6A: case 0x6B: case 0x6C: { // i32.add / i32.sub / i32.mul
			const Value rhs = popValue();
			const Value lhs = popValue();
			const uint32_t x = static_cast<uint32_t>(lhs.i32);
			const uint32_t y = static_cast<uint32_t>(rhs.i32);
			uint32_t result = 0;
			if (opcode == 0x6A)      result = x + y;
			else if (opcode == 0x6B) result = x - y;
			else                     result = x * y;
			Value v{}; v.i32 = static_cast<int32_t>(result); v.kind = ValueTypeCode::I32;
			stack.push_back(v);
			break;
		}
		case 0x7C: case 0x7D: case 0x7E: { // i64.add / i64.sub / i64.mul
			const Value rhs = popValue();
			const Value lhs = popValue();
			const uint64_t x = static_cast<uint64_t>(lhs.i64);
			const uint64_t y = static_cast<uint64_t>(rhs.i64);
			uint64_t result = 0;
			if (opcode == 0x7C)      result = x + y;
			else if (opcode == 0x7D) result = x - y;
			else                     result = x * y;
			Value v{}; v.i64 = static_cast<int64_t>(result); v.kind = ValueTypeCode::I64;
			stack.push_back(v);
			break;
		}


		case 0x23: { // global.get
			const uint32_t globalIdx = stream.readLEB128<uint32_t>();
			if (globalIdx >= internals.globalsStorage.size())
				throw std::runtime_error("global.get in constant expr: index out of range");
			stack.push_back(internals.globalsStorage[globalIdx]);
			break;
		}

		case 0xD0: { // ref.null <heaptype>
			(void)stream.readLEB128<int32_t>();
			Value v{}; v.ref = nullptr; v.kind = ValueTypeCode::FuncRef;
			stack.push_back(v);
			break;
		}

		case 0xD2: { // ref.func <funcidx>: biased-index sentinel, resolved later
			const uint32_t funcIdx = stream.readLEB128<uint32_t>();
			Value v{}; v.ref = reinterpret_cast<void*>(encodeRefFuncIndex(funcIdx)); v.kind = ValueTypeCode::FuncRef;
			stack.push_back(v);
			break;
		}

		case 0xFB: { // GC prefix
			const uint32_t sub = stream.readLEB128<uint32_t>();
			switch (static_cast<GCOpcode>(sub)) {
			case GCOpcode::RefI31: {
				const uint32_t raw = static_cast<uint32_t>(popValue().i32);
				Value v{}; v.ref = reinterpret_cast<void*>(static_cast<uintptr_t>((raw << 1u) | 1u)); v.kind = ValueTypeCode::I31Ref;
				stack.push_back(v);
				break;
			}
			case GCOpcode::AnyConvertExtern:
			case GCOpcode::ExternConvertAny:
				stack.push_back(popValue()); // representation-preserving
				break;

			case GCOpcode::StructNewDefault: {
				const uint32_t typeIdx = stream.readLEB128<uint32_t>();
				const StructType& st = std::get<StructType>(module->types[typeIdx].composite);
				Value v{}; v.ref = allocateStructObject(static_cast<uint32_t>(structSize(st)), typeIdx); v.kind = ValueTypeCode::StructRef;
				stack.push_back(v);
				break;
			}
			case GCOpcode::StructNew: {
				const uint32_t typeIdx = stream.readLEB128<uint32_t>();
				const StructType& st = std::get<StructType>(module->types[typeIdx].composite);
				const std::vector<size_t> offsets = structFieldOffsets(st);
				std::vector<Value> fields(st.fields.size());
				for (size_t i = st.fields.size(); i-- > 0; )
					fields[i] = popValue();
				void* object = allocateStructObject(static_cast<uint32_t>(structSize(st)), typeIdx);
				for (size_t i = 0; i < st.fields.size(); ++i)
					storeValue(object, offsets[i], st.fields[i].storageType, fields[i]);
				Value v{}; v.ref = object; v.kind = ValueTypeCode::StructRef;
				stack.push_back(v);
				break;
			}

			case GCOpcode::ArrayNewDefault: {
				const uint32_t typeIdx = stream.readLEB128<uint32_t>();
				const ArrayType& at = std::get<ArrayType>(module->types[typeIdx].composite);
				const uint32_t length = static_cast<uint32_t>(popValue().i32);
				Value v{}; v.ref = allocateArrayObject(static_cast<uint32_t>(arraySize()),
					static_cast<uint32_t>(storageBytes(at.elementType.storageType)), length, typeIdx);
				v.kind = ValueTypeCode::ArrayRef;
				stack.push_back(v);
				break;
			}
			case GCOpcode::ArrayNew: {
				const uint32_t typeIdx = stream.readLEB128<uint32_t>();
				const ArrayType& at = std::get<ArrayType>(module->types[typeIdx].composite);
				const uint32_t length = static_cast<uint32_t>(popValue().i32);
				const Value init = popValue();
				void* object = allocateArrayObject(static_cast<uint32_t>(arraySize()),
					static_cast<uint32_t>(storageBytes(at.elementType.storageType)), length, typeIdx);
				uint8_t* data = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(object) + kArrayDataOffset);
				const size_t elementSize = storageBytes(at.elementType.storageType);
				for (uint32_t i = 0; i < length; ++i)
					storeValue(data + static_cast<size_t>(i) * elementSize, 0, at.elementType.storageType, init);
				Value v{}; v.ref = object; v.kind = ValueTypeCode::ArrayRef;
				stack.push_back(v);
				break;
			}
			case GCOpcode::ArrayNewFixed: {
				const uint32_t typeIdx = stream.readLEB128<uint32_t>();
				const uint32_t count = stream.readLEB128<uint32_t>();
				const ArrayType& at = std::get<ArrayType>(module->types[typeIdx].composite);
				std::vector<Value> values(count);
				for (size_t i = count; i-- > 0; )
					values[i] = popValue();
				void* object = allocateArrayObject(static_cast<uint32_t>(arraySize()),
					static_cast<uint32_t>(storageBytes(at.elementType.storageType)), count, typeIdx);
				uint8_t* data = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(object) + kArrayDataOffset);
				const size_t elementSize = storageBytes(at.elementType.storageType);
				for (uint32_t i = 0; i < count; ++i)
					storeValue(data + static_cast<size_t>(i) * elementSize, 0, at.elementType.storageType, values[i]);
				Value v{}; v.ref = object; v.kind = ValueTypeCode::ArrayRef;
				stack.push_back(v);
				break;
			}

			default:
				throw InvalidOpcodeException(0xFB0000u | sub);
			}
			break;
		}

		default:
			throw InvalidOpcodeException(opcode);
		}
	}

	if (stack.empty())
		throw std::runtime_error("empty constant expression");
	return stack.back();
}

int64_t ModuleInstance::growMemory(uint32_t memIdx, uint64_t deltaPages) {
	if (memIdx >= internals.memoryRefs.size())
		return -1;

	LinearMemory* memory = internals.memoryRefs[memIdx];
	// 64-bit throughout: a memory64 can grow past 2^32 pages, and the caller
	// reports the old page count in the memory's own index type
	// (docs/proposals/MEMORY64.md M64-1).
	const uint64_t oldPages = memory->memorySize / 0x10000;

	// The Store owns the memory (and therefore its growth policy/limits).
	if (!Store::growMemory(memory, deltaPages))
		return -1; // Caller turns this into a Wasm trap

	return static_cast<int64_t>(oldPages);
}

bool ModuleInstance::growTable(uint32_t tableIdx, uint64_t deltaEntries) {
	if (tableIdx >= internals.tableRefs.size())
		return false;

	// The Store owns the table (and therefore its growth policy/limits).
	// Growth may move the slot array; the TableInstance view is updated in
	// place, so ctx.tables (which points at the view) needs no re-sync.
	return Store::growTable(internals.tableRefs[tableIdx], deltaEntries);
}

void ModuleInstance::memoryInit(uint32_t memIdx, uint32_t dataIdx, uint32_t dstOffset, uint32_t srcOffset, uint32_t len)
{
	if (memIdx >= internals.memoryRefs.size())
		trap("memory index out of range");
	if (dataIdx >= module->dataSegments.size())
		trap("data segment index out of range");

	LinearMemory* memory = internals.memoryRefs[memIdx];
	const DataSegment& seg = module->dataSegments[dataIdx];

	// A dropped segment behaves as if it were empty: a zero-length copy is a
	// no-op, anything longer traps. See docs/proposals/BULK_MEMORY.md BM-1/BM-3.
	const uint64_t segmentSize = internals.dataSegmentDropped[dataIdx] ? 0 : seg.data.size();
	if (static_cast<uint64_t>(srcOffset) + len > segmentSize)
		trap("out of bounds memory.init source");
	if (static_cast<uint64_t>(dstOffset) + len > memory->memorySize)
		trap("out of bounds memory access");

	if (len == 0)
		return;
	std::memcpy(memory->memoryBase + dstOffset, seg.data.data() + srcOffset, len);
}

void ModuleInstance::dataDrop(uint32_t dataIdx)
{
	if (dataIdx >= internals.dataSegmentDropped.size())
		trap("data segment index out of range");
	internals.dataSegmentDropped[dataIdx] = true;
}

void ModuleInstance::tableInit(uint32_t elemIdx, uint32_t tableIdx, uint32_t dstOffset, uint32_t srcOffset, uint32_t len)
{
	if (elemIdx >= module->elementSegments.size())
		trap("element segment index out of range");
	if (tableIdx >= internals.tableRefs.size())
		trap("table index out of range");

	const ElementSegment& seg = module->elementSegments[elemIdx];
	// Dropped -> empty segment, exactly like memoryInit above.
	const size_t entryCount = internals.elementSegmentDropped[elemIdx]
		? 0
		: (seg.initIndices.empty() ? seg.initExprs.size() : seg.initIndices.size());
	if (static_cast<uint64_t>(srcOffset) + len > entryCount)
		trap("out of bounds table.init source");
	TableInstance* table = internals.tableRefs[tableIdx];
	if (static_cast<uint64_t>(dstOffset) + len > table->size)
		trap("out of bounds table access");
	if (len == 0)
		return;

	const uint32_t importedFuncCount =
		static_cast<uint32_t>(module->importFunctions.size());

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
			// Expression form: use the reference evaluated once at
			// instantiation (never re-evaluate the expression here).
			callable = callableFromSegmentRef(internals.elementSegmentRefs[elemIdx][entryIndex]);
		}
		table->base[dstOffset + i] = callable;
	}
}

void ModuleInstance::elemDrop(uint32_t elemIdx)
{
	if (elemIdx >= internals.elementSegmentDropped.size())
		trap("element segment index out of range");
	internals.elementSegmentDropped[elemIdx] = true;
}

void ModuleInstance::bufferInitFromData(uint32_t dataIdx, void* dst, uint32_t srcOffset, uint32_t lenBytes)
{
	if (dataIdx >= module->dataSegments.size())
		trap("data segment index out of range");

	const DataSegment& seg = module->dataSegments[dataIdx];
	// Dropped segment == empty segment (BULK_MEMORY.md BM-1/BM-3).
	const uint64_t segmentSize = internals.dataSegmentDropped[dataIdx] ? 0 : seg.data.size();
	if (static_cast<uint64_t>(srcOffset) + lenBytes > segmentSize)
		trap("out of bounds data segment access");
	if (lenBytes == 0)
		return;
	std::memcpy(dst, seg.data.data() + srcOffset, lenBytes);
}

void ModuleInstance::bufferInitFromElems(uint32_t elemIdx, void* dst, uint32_t srcOffset, uint32_t lenElems)
{
	if (elemIdx >= module->elementSegments.size())
		trap("element segment index out of range");

	const ElementSegment& seg = module->elementSegments[elemIdx];
	// Dropped segment == empty segment (BULK_MEMORY.md BM-1/BM-3).
	const size_t entryCount = internals.elementSegmentDropped[elemIdx]
		? 0
		: (seg.initIndices.empty() ? seg.initExprs.size() : seg.initIndices.size());
	if (static_cast<uint64_t>(srcOffset) + lenElems > entryCount)
		trap("out of bounds element segment access");
	if (lenElems == 0)
		return;

	Callable** out = static_cast<Callable**>(dst);
	const uint32_t importedFuncCount =
		static_cast<uint32_t>(module->importFunctions.size());

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
			// Expression form: use the reference evaluated once at
			// instantiation (never re-evaluate the expression here).
			callable = callableFromSegmentRef(internals.elementSegmentRefs[elemIdx][entryIndex]);
		}
		out[i] = callable;
	}
}

void* ModuleInstance::allocateStructObject(uint32_t size, uint32_t localTypeIdx)
{
	// Resolve the module-local index against this instance's module and stamp the
	// canonical TypeId into the header and the lookup map, so a later cast can
	// match it even from another module (M6).
	const TypeId typeId = module->typeId(LocalTypeIdx{localTypeIdx});

	// A GC object is host memory the module asks for; charge it against the
	// Store's allocation budget so a failure is a Wasm trap, never an abort
	// (docs/SAFETY.md). GC objects are never freed, so the charge is one-way.
	if (!Store::global().reserveBytes(size))
		trap("out of memory");

	uint8_t* object = static_cast<uint8_t*>(std::calloc(1, size));
	if (!object)
		trap("out of memory");
	*reinterpret_cast<uint32_t*>(object + GcHeader::kTypeIdOffset) = typeId.value;
	// A fresh object is owned by its creator: the count starts at 1
	// (docs/proposals/GC.md D14).
	*reinterpret_cast<uint32_t*>(object + GcHeader::kRefcountOffset) = 1;
	internals.gcObjectTypes[object] = typeId;
	++internals.gcAllocatedObjects;
	return object;
}

void* ModuleInstance::allocateArrayObject(uint32_t headerSize, uint32_t elementSize, uint32_t length, uint32_t localTypeIdx)
{
	const TypeId typeId = module->typeId(LocalTypeIdx{localTypeIdx});

	// Charge the whole object (header + elements) *before* allocating: an
	// `array.new` with an absurd runtime length must trap, not try to
	// `calloc` tens of gigabytes (docs/SAFETY.md, docs/proposals/GC.md C7).
	const uint64_t totalBytes = static_cast<uint64_t>(headerSize) +
		static_cast<uint64_t>(elementSize) * static_cast<uint64_t>(length);
	if (!Store::global().reserveBytes(totalBytes))
		trap("out of memory");

	uint8_t* header = static_cast<uint8_t*>(std::calloc(1, headerSize));
	if (!header)
		trap("out of memory");

	uint8_t* data = nullptr;
	if (length != 0) {
		data = static_cast<uint8_t*>(std::calloc(length, elementSize));
		if (!data)
			trap("out of memory");
	}

	*reinterpret_cast<uint32_t*>(header + GcHeader::kTypeIdOffset) = typeId.value;
	*reinterpret_cast<uint32_t*>(header + GcHeader::kRefcountOffset) = 1;
	*reinterpret_cast<uint32_t*>(header + GcHeader::kArrayLengthOffset) = length;
	*reinterpret_cast<void**>(header + GcHeader::kArrayDataOffset) = data;
	internals.gcObjectTypes[header] = typeId;
	++internals.gcAllocatedObjects;
	return header;
}

bool ModuleInstance::tryGetGcTypeId(const void* ref, TypeId& typeId) const
{
	auto it = internals.gcObjectTypes.find(ref);
	if (it == internals.gcObjectTypes.end())
		return false;
	typeId = it->second;
	return true;
}

bool ModuleInstance::refMatchesHeapType(const void* ref, const HeapType& heapType, bool nullable) const
{
	const TypeRegistry& registry = Store::global().types();

	// Canonical form of the expected heap type. A module-local index is resolved
	// against the module that contains the cast instruction.
	CanonHeapType expected;
	if (heapType.isTypeIndex) {
		expected.kind = CanonHeapType::Kind::Global;
		expected.id = module->typeId(LocalTypeIdx{heapType.typeIndex});
	} else {
		expected.kind = CanonHeapType::Kind::Abstract;
		expected.abstract = heapType.abstract;
	}

	if (ref == nullptr)
		return nullable; // null matches only nullable target types

	// Unboxed i31 immediate (low bit set).
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

	// Function references carry their canonical type id.
	const auto matchCallable = [&registry, &expected, &heapType](const Callable& callable) -> bool {
		if (callable.typeId.isNone()) {
			// Native import without a canonical type: only abstract classification.
			if (heapType.isTypeIndex)
				return false;
			switch (heapType.abstract) {
			case AbstractHeapType::Any:
			case AbstractHeapType::Func:
				return true;
			default:
				return false;
			}
		}
		CanonHeapType actual;
		actual.kind = CanonHeapType::Kind::Global;
		actual.id = callable.typeId;
		return registry.matchesHeap(actual, expected);
	};
	for (const Callable& callable : internals.importStorage)
		if (&callable == ref)
			return matchCallable(callable);
	for (const Callable& callable : internals.internalCallables)
		if (&callable == ref)
			return matchCallable(callable);

	// GC objects carry their canonical type id.
	TypeId gcTypeId;
	if (tryGetGcTypeId(ref, gcTypeId)) {
		CanonHeapType actual;
		actual.kind = CanonHeapType::Kind::Global;
		actual.id = gcTypeId;
		return registry.matchesHeap(actual, expected);
	}

	// Unknown reference: only the top abstract types match.
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

// ── Reference counting (docs/proposals/GC.md §6, RC-1/RC-3/RC-7) ────────────
//
// The count lives in the object header (GcHeader::kRefcountOffset) and is
// touched through std::atomic_ref so the Threads work can share an object later
// (D16) without changing the layout. `i31` immediates (low bit set),
// `externref`s and `funcref`s have no header, so they are never counted;
// `gcRefcountSlot` is what tells a GC object apart from every other reference.

uint32_t* ModuleInstance::gcRefcountSlot(const void* ref)
{
	if (ref == nullptr)
		return nullptr;
	// Unboxed i31 immediate (low bit set) — not a heap pointer.
	if ((reinterpret_cast<uintptr_t>(ref) & 1u) != 0)
		return nullptr;
	// Only a pointer this instance allocated *and registered* is a GC object.
	if (internals.gcObjectTypes.find(ref) == internals.gcObjectTypes.end())
		return nullptr;
	return reinterpret_cast<uint32_t*>(
			static_cast<uint8_t*>(const_cast<void*>(ref)) + GcHeader::kRefcountOffset);
}

uint32_t ModuleInstance::gcRefCount(const void* ref) const
{
	if (ref == nullptr || (reinterpret_cast<uintptr_t>(ref) & 1u) != 0
			|| internals.gcObjectTypes.find(ref) == internals.gcObjectTypes.end())
		return 0;
	auto* slot = const_cast<uint32_t*>(reinterpret_cast<const uint32_t*>(
			static_cast<const uint8_t*>(ref) + GcHeader::kRefcountOffset));
	return std::atomic_ref<uint32_t>(*slot).load(std::memory_order_relaxed);
}

void* ModuleInstance::refClone(void* ref)
{
	uint32_t* slot = gcRefcountSlot(ref);
	if (slot == nullptr)
		return ref;
	std::atomic_ref<uint32_t> count(*slot);
	uint32_t old = count.load(std::memory_order_relaxed);
	for (;;) {
		if (old == std::numeric_limits<uint32_t>::max())
			break; // saturate: a leak is preferable to a wrapped count (D19)
		if (count.compare_exchange_weak(old, old + 1, std::memory_order_relaxed))
			break;
	}
	return ref;
}

void ModuleInstance::refDrop(void* ref)
{
	uint32_t* slot = gcRefcountSlot(ref);
	if (slot == nullptr)
		return;
	std::atomic_ref<uint32_t> count(*slot);
	uint32_t old = count.load(std::memory_order_relaxed);
	for (;;) {
		if (old == 0)
			trap("GC reference count underflow"); // D19: trap, never wrap
		if (count.compare_exchange_weak(old, old - 1, std::memory_order_relaxed)) {
			if (old == 1)
				reclaimGcObject(ref);
			break;
		}
	}
}

void ModuleInstance::refStore(void** slot, void* newRef)
{
	void* old = *slot;
	refClone(newRef);   // retain the new value first (self-assignment safe)
	*slot = newRef;
	refDrop(old);       // then release the value we overwrote
}

void ModuleInstance::reclaimGcObject(void* ref)
{
	// The count reached zero. Reclamation is *deferred* for this milestone:
	// Wasm locals and operand-stack temporaries can still hold uncounted
	// borrows, so freeing here would be a use-after-free. Record the transition
	// instead; the instance's region teardown (GC-2) reclaims memory wholesale.
	// Turning this into "free + credit the Store budget" is the ownership work
	// that follows (docs/proposals/GC.md §7, RC-1b/RC-5).
	(void)ref;
	++internals.gcReclaimedObjects;
}

}
