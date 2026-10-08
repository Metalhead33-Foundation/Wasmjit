#include "WasmModuleInstance.hpp"
#include "WasmException.hpp"
#include "WasmOpcode.hpp"
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

			// Write the callable pointer into the store-owned table slot.
			table->base[tableOffset + i] = callable;
		}
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

	// Imported globals and tags follow the same resolve-then-assign pattern;
	// they are handled elsewhere (globals) or not yet wired (tags).
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
	// Imported tables occupy the low indices of the table index space; they
	// were already appended to tableRefs by resolveImports(). The module's own
	// tables follow here, so ctx.tables ends up pointing at the full space.
	internals.tableRefs.reserve(module->importTables.size() + module->tables.size());
	for (const TableType& tableType : module->tables) {
		const uint64_t initialSize = tableType.limits.initial;
		const uint64_t maxSize = tableType.limits.maximum.has_value()
			? tableType.limits.maximum.value()
			: UINT64_MAX;

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
//   struct: uint32 header, then naturally-aligned fields;
//   array:  uint32 header, uint32 length, pointer to the element buffer.
// Keep in sync with translateStruct / translateArray.
constexpr size_t kGcHeaderBytes = 4;
constexpr size_t kArrayDataOffset = 8;

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

		case 0xD2: { // ref.func <funcidx>: unresolved sentinel, fixed in applyActiveSegments
			const uint32_t funcIdx = stream.readLEB128<uint32_t>();
			Value v{}; v.ref = reinterpret_cast<void*>(static_cast<uintptr_t>(funcIdx)); v.kind = ValueTypeCode::FuncRef;
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

bool ModuleInstance::growTable(uint32_t tableIdx, uint32_t deltaEntries) {
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

void ModuleInstance::tableInit(uint32_t elemIdx, uint32_t tableIdx, uint32_t dstOffset, uint32_t srcOffset, uint32_t len)
{
	if (elemIdx >= module->elementSegments.size())
		std::abort();
	if (internals.elementSegmentDropped[elemIdx])
		std::abort();
	if (tableIdx >= internals.tableRefs.size())
		std::abort();

	const ElementSegment& seg = module->elementSegments[elemIdx];
	const size_t entryCount = seg.initIndices.empty() ? seg.initExprs.size() : seg.initIndices.size();
	if (srcOffset > entryCount || len > entryCount - srcOffset)
		std::abort();
	TableInstance* table = internals.tableRefs[tableIdx];
	if (static_cast<uint64_t>(dstOffset) + len > table->size)
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
		table->base[dstOffset + i] = callable;
	}
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

void* ModuleInstance::allocateStructObject(uint32_t size, uint32_t localTypeIdx)
{
	// Resolve the module-local index against this instance's module and stamp the
	// canonical TypeId into the header and the lookup map, so a later cast can
	// match it even from another module (M6).
	const TypeId typeId = module->typeId(LocalTypeIdx{localTypeIdx});

	uint8_t* object = static_cast<uint8_t*>(std::calloc(1, size));
	if (!object)
		std::abort();
	*reinterpret_cast<uint32_t*>(object) = typeId.value;
	internals.gcObjectTypes[object] = typeId;
	return object;
}

void* ModuleInstance::allocateArrayObject(uint32_t headerSize, uint32_t elementSize, uint32_t length, uint32_t localTypeIdx)
{
	const TypeId typeId = module->typeId(LocalTypeIdx{localTypeIdx});

	uint8_t* header = static_cast<uint8_t*>(std::calloc(1, headerSize));
	if (!header)
		std::abort();

	uint8_t* data = nullptr;
	if (length != 0) {
		data = static_cast<uint8_t*>(std::calloc(length, elementSize));
		if (!data)
			std::abort();
	}

	*reinterpret_cast<uint32_t*>(header) = typeId.value;
	*reinterpret_cast<uint32_t*>(header + sizeof(uint32_t)) = length;
	*reinterpret_cast<void**>(header + sizeof(uint32_t) * 2) = data;
	internals.gcObjectTypes[header] = typeId;
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

}
