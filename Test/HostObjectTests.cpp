#include <catch2/catch_all.hpp>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

#include "WasmBase/WasmGcHeader.hpp"
#include "WasmBase/WasmHostObject.hpp"
#include "WasmBase/WasmHostType.hpp"
#include "WasmBase/WasmRegistryImportResolver.hpp"
#include "WasmBase/WasmRuntime.hpp"
#include "WasmBase/WasmStore.hpp"
#include "WasmBase/WasmTrap.hpp"

#ifndef WASM_TEST_DIR
//! Fixture directory; the build defines it, this keeps a standalone compile honest.
#define WASM_TEST_DIR ""
#endif

namespace {

//! A struct that exercises every layout rule in one definition: a scalar, an
//! 8-byte alignment, a 16-byte alignment, a pointer-sized reference and a packed
//! byte. The expected offsets are the same numbers `[tiers]` pins for the GC
//! contract, so a drift between the scaffold and the heap shows up here.
WASM::HostTypeDefinition mixedStruct()
{
	return WASM::hostStruct({
		WASM::hostValueField(WASM::ValueTypeCode::I32),
		WASM::hostValueField(WASM::ValueTypeCode::I64),
		WASM::hostValueField(WASM::ValueTypeCode::V128),
		WASM::hostReferenceField(WASM::ValueTypeCode::ExternRef),
		WASM::hostPackedField(WASM::ValueTypeCode::I8),
	});
}

//! A leaf struct: the shape the probe module declares and the host interns.
WASM::HostTypeDefinition leafStruct()
{
	return WASM::hostStruct({
		WASM::hostValueField(WASM::ValueTypeCode::I32),
		WASM::hostValueField(WASM::ValueTypeCode::I32),
	});
}

//! A holder: one mutable `anyref`, the shape the probe module declares as
//! `$holder` (`SO-6`: a host object that is a graph node).
WASM::HostTypeDefinition holderStruct()
{
	return WASM::hostStruct({
		WASM::hostReferenceField(WASM::ValueTypeCode::AnyRef,
				/*isNullable*/ true, /*isMutable*/ true),
	});
}

} // namespace

// The scaffold's one implemented computation is the layout: it must agree with
// gcStructFieldOffset/gcStructSize, which the runtime and every backend share
// (docs/ABI.md "GC Object and Heap ABI", A.14.5's offset rule).
TEST_CASE("a host struct lays out like the GC contract", "[hostobject][layout]")
{
	const WASM::HostTypeDefinition definition = mixedStruct();
	const WASM::HostTypeLayout layout = WASM::layoutHostType(definition);

	CHECK(layout.headerBytes == WASM::GcHeader::kHeaderBytes);
	CHECK(layout.elementBytes == 0);
	REQUIRE(layout.fields.size() == 5);

	// i32 at 16; i64 re-aligns to 24; v128 to 32; a pointer after 16 bytes at 48;
	// a packed i8 needs no padding at 56.
	CHECK(layout.fields[0].offset == 16);
	CHECK(layout.fields[1].offset == 24);
	CHECK(layout.fields[2].offset == 32);
	CHECK(layout.fields[3].offset == 48);
	CHECK(layout.fields[4].offset == 56);

	CHECK(layout.fields[0].bytes == 4);
	CHECK(layout.fields[1].bytes == 8);
	CHECK(layout.fields[2].bytes == 16);
	CHECK(layout.fields[3].bytes == 8);
	CHECK(layout.fields[4].bytes == 1);

	// The reference is flagged so a data accessor can refuse it (SOD4); the
	// value fields are not.
	CHECK_FALSE(layout.fields[0].isReference);
	CHECK_FALSE(layout.fields[1].isReference);
	CHECK_FALSE(layout.fields[2].isReference);
	CHECK(layout.fields[3].isReference);
	CHECK_FALSE(layout.fields[4].isReference);

	// Padded to the strictest alignment (v128).
	CHECK(layout.totalBytes == 64);
}

TEST_CASE("a host array reports its element base and stride", "[hostobject][layout]")
{
	const WASM::HostTypeLayout i64Array = WASM::layoutHostType(
			WASM::hostArray(WASM::hostValueField(WASM::ValueTypeCode::I64)));
	CHECK(i64Array.headerBytes == WASM::GcHeader::kArrayHeaderBytes);
	REQUIRE(i64Array.fields.size() == 1);
	CHECK(i64Array.fields[0].offset == 16);
	CHECK(i64Array.elementBytes == 8);
	CHECK(i64Array.fields[0].bytes == 8);
	CHECK(i64Array.totalBytes == 16);

	const WASM::HostTypeLayout vectorArray = WASM::layoutHostType(
			WASM::hostArray(WASM::hostValueField(WASM::ValueTypeCode::V128)));
	REQUIRE(vectorArray.fields.size() == 1);
	CHECK(vectorArray.fields[0].offset == 16);
	CHECK(vectorArray.elementBytes == 16);

	const WASM::HostTypeLayout byteArray = WASM::layoutHostType(
			WASM::hostArray(WASM::hostPackedField(WASM::ValueTypeCode::I8)));
	REQUIRE(byteArray.fields.size() == 1);
	CHECK(byteArray.fields[0].offset == 12);
	CHECK(byteArray.elementBytes == 1);

	const WASM::HostTypeLayout refArray = WASM::layoutHostType(
			WASM::hostArray(WASM::hostReferenceField(WASM::ValueTypeCode::AnyRef)));
	REQUIRE(refArray.fields.size() == 1);
	CHECK(refArray.fields[0].isReference);
	CHECK(refArray.elementBytes == sizeof(void*));
}

// The definition is the only thing the engine may canonicalize, so everything a
// Wasm struct/array cannot express is refused before any state changes (A.14.1).
TEST_CASE("a definition outside the Wasm vocabulary is refused", "[hostobject][layout]")
{
	using WASM::ValueTypeCode;
	CHECK_THROWS_AS(WASM::layoutHostType(
			WASM::hostStruct({WASM::hostPackedField(ValueTypeCode::I32)})),
					std::invalid_argument);
	CHECK_THROWS_AS(WASM::layoutHostType(
			WASM::hostStruct({WASM::hostValueField(ValueTypeCode::I8)})),
					std::invalid_argument);
	CHECK_THROWS_AS(WASM::layoutHostType(
			WASM::hostStruct({WASM::hostValueField(ValueTypeCode::StringRef)})),
					std::invalid_argument);
	CHECK_THROWS_AS(WASM::layoutHostType(
			WASM::hostStruct({WASM::hostValueField(ValueTypeCode::Void)})),
					std::invalid_argument);

	WASM::HostTypeDefinition arrayWithFields =
			WASM::hostArray(WASM::hostValueField(ValueTypeCode::I32));
	arrayWithFields.fields.push_back(WASM::hostValueField(ValueTypeCode::I32));
	CHECK_THROWS_AS(WASM::layoutHostType(arrayWithFields), std::invalid_argument);

	// A malformed definition is rejected before the stub, so the error names the
	// real problem rather than the missing implementation.
	WASM::Store store;
	CHECK_THROWS_AS(WASM::internHostType(store,
			WASM::hostStruct({WASM::hostValueField(ValueTypeCode::I8)})),
					std::invalid_argument);
}

// The seam is available (`SO-1`), and its one staging restriction is stated by
// the interner rather than discovered from a silently leaking object (`SO17`).
TEST_CASE("the host-type seam is available and refuses bridge-owned references", "[hostobject]")
{
	CHECK(WASM::hostTypeSupportAvailable());

	WASM::Store store;
	// An id the registry does not know is not a core type.
	CHECK_THROWS_AS(WASM::allocateHostStruct(store, WASM::TypeId{0}), std::invalid_argument);
	CHECK_THROWS_AS(WASM::allocateHostArray(store, WASM::TypeId{0}, 4), std::invalid_argument);

	// A GC reference is the Store's own count, so it interns (`SO-6`); an
	// `externref` belongs to a host bridge the Store does not own (`SO18`).
	CHECK_THROWS_AS(WASM::internHostType(store, mixedStruct()), std::logic_error);
	CHECK_THROWS_AS(WASM::HostType::intern(store, mixedStruct()), std::logic_error);
	CHECK_NOTHROW(WASM::internHostType(store, holderStruct()));
	CHECK_NOTHROW(WASM::internHostType(store, WASM::hostStruct({
			WASM::hostReferenceField(WASM::ValueTypeCode::FuncRef)})));

	const WASM::TypeId leaf = WASM::internHostType(store, leafStruct());
	// The wrong composite is refused, in both directions.
	CHECK_THROWS_AS(WASM::allocateHostArray(store, leaf, 4), std::invalid_argument);
	const WASM::TypeId array = WASM::internHostType(store,
			WASM::hostArray(WASM::hostValueField(WASM::ValueTypeCode::I32)));
	CHECK_THROWS_AS(WASM::allocateHostStruct(store, array), std::invalid_argument);
}

TEST_CASE("HostType describes a type and interns it", "[hostobject]")
{
	// A description needs no Store: the layout is derived from the definition.
	WASM::HostType described(leafStruct());
	CHECK_FALSE(described.isInterned());
	CHECK(described.id().isNone());
	CHECK(described.layout().totalBytes == 24);
	CHECK(described.fieldOffset(0) == 16);
	CHECK(described.fieldOffset(1) == 20);
	CHECK_THROWS_AS(described.field(2), std::out_of_range);

	WASM::Store store;
	const WASM::HostType type = WASM::HostType::intern(store, leafStruct());
	REQUIRE(type.isInterned());
	CHECK_FALSE(type.id().isNone());

	// Structural identity (SOD2): the same shape is the same `TypeId`, and a
	// changed shape -- a field mutability counts -- is a different one.
	CHECK(WASM::internHostType(store, leafStruct()) == type.id());
	WASM::HostTypeDefinition mutableDefinition = leafStruct();
	mutableDefinition.fields[0].isMutable = true;
	CHECK(WASM::internHostType(store, mutableDefinition) != type.id());
}

// The ergonomic array surface: the element base and stride come from the same
// layout a module's `array.*` lowering uses, so a host element and a guest
// element are the same bytes.
TEST_CASE("a host array carries its elements", "[hostobject]")
{
	WASM::Store store;
	const WASM::HostType type = WASM::HostType::intern(store,
			WASM::hostArray(WASM::hostValueField(WASM::ValueTypeCode::I32)));
	REQUIRE(type.isInterned());

	WASM::HostObject array = WASM::HostObject::createArray(store, type, 4);
	REQUIRE(array.isValid());
	CHECK(array.length() == 4);
	// `i32` array: the 12-byte header is already 4-byte aligned, so the elements
	// start at 12 and each is 4 bytes.
	CHECK(array.fieldOffset(0) == 12);
	CHECK(array.elementOffset(0) == 12);
	CHECK(array.elementOffset(3) == 24);
	CHECK_THROWS_AS(array.elementOffset(4), std::out_of_range);

	array.writeElement<std::int32_t>(0, 11);
	array.writeElement<std::int32_t>(3, 33);
	CHECK(array.readElement<std::int32_t>(0) == 11);
	CHECK(array.readElement<std::int32_t>(3) == 33);
	// A wrong-sized type is refused by the same check a field uses.
	CHECK_THROWS_AS(array.readElement<std::int64_t>(0), std::logic_error);

	// A struct is not an array, and an empty handle is not either.
	const WASM::HostType structType = WASM::HostType::intern(store, leafStruct());
	WASM::HostObject object = WASM::HostObject::createStruct(store, structType);
	REQUIRE(object.isValid());
	CHECK_THROWS_AS(object.elementOffset(0), std::logic_error);
	array.reset();
	CHECK_FALSE(array.isValid());
	CHECK_THROWS_AS(array.elementOffset(0), std::logic_error);
}

TEST_CASE("HostObject is a safe empty handle", "[hostobject]")
{
	WASM::HostObject object;
	CHECK_FALSE(object.isValid());
	CHECK(object.raw() == nullptr);
	CHECK(object.store() == nullptr);
	CHECK(object.typeId().isNone());
	CHECK(object.length() == 0);
	CHECK_NOTHROW(object.reset());

	CHECK_THROWS_AS(object.fieldOffset(0), std::logic_error);
	CHECK_THROWS_AS(object.field(0), std::logic_error);
	CHECK_THROWS_AS(object.readField<std::uint32_t>(0), std::logic_error);
	CHECK_THROWS_AS(object.writeField<std::uint32_t>(0, 1), std::logic_error);

	// A move leaves both handles empty and never double-releases.
	WASM::HostObject moved(std::move(object));
	CHECK_FALSE(moved.isValid());
	CHECK_FALSE(object.isValid());
	WASM::HostObject assigned;
	assigned = std::move(moved);
	CHECK_FALSE(assigned.isValid());
	CHECK_FALSE(moved.isValid());

	WASM::Store store;
	WASM::HostType type(mixedStruct());
	CHECK_THROWS_AS(WASM::HostObject::createStruct(store, type), std::invalid_argument);
}

// `SO-1`'s acceptance case: the host interns the same shape a module declares,
// allocates the object, and the guest both reads it and keeps it alive by
// storing it. The counts are the engine's, so `liveObjectCount()` pins them.
TEST_CASE("a host-created object is a guest object (SO-1)", "[hostobject][gc]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	WASM::Store& store = runtime->store();

	const std::filesystem::path path =
			std::filesystem::path(WASM_TEST_DIR) / "host_object_probe.wasm";
	auto module = runtime->loadFile(path);
	REQUIRE(module != nullptr);
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	const WASM::HostType type = WASM::HostType::intern(store, leafStruct());
	REQUIRE(type.isInterned());
	// Dedup across the boundary: the host's type *is* the module's type. `$pair`
	// is the module's first declared type, so its local index is 0.
	CHECK(type.id() == module->typeId(WASM::LocalTypeIdx{0}));

	const std::size_t before = store.gc().liveObjectCount();
	{
		WASM::HostObject object = WASM::HostObject::createStruct(store, type);
		REQUIRE(object.isValid());
		CHECK(object.store() == &store);
		CHECK(object.typeId() == type.id());
		CHECK(object.fieldOffset(0) == 16);
		CHECK(object.field(1).bytes == 4);
		object.writeField<std::int32_t>(0, 20);
		object.writeField<std::int32_t>(1, 22);
		CHECK(object.readField<std::int32_t>(1) == 22);

		// `ref.test`, `struct.get` and the write barrier all see the object the
		// host allocated, under the module's own type identity.
		CHECK(runtime->callExport<std::int32_t>(instance, "sum_pair", object.raw()) == 42);
		CHECK(runtime->callExport<std::int32_t>(instance, "is_pair", object.raw()) == 1);

		// The guest stores it, so its global takes a count: releasing the host
		// root below must not recycle the object.
		CHECK(runtime->callExport<std::int32_t>(instance, "save", object.raw()) == 1);
	}
	runtime->safepoint();
	CHECK(store.gc().liveObjectCount() == before + 1);

	// The guest drops its count; the instance queues the object at `refDrop` and
	// its own safepoint recycles it.
	CHECK(runtime->callExport<std::int32_t>(instance, "drop_saved") == 1);
	runtime->safepoint();
	CHECK(store.gc().liveObjectCount() == before);
}

// `SO-6`'s acceptance case: a host object is a graph node. The guest stores a
// reference into it, the host releases its root, and the safepoint walk drops the
// field's count — so both objects come back to the baseline.
TEST_CASE("a released host object drops the references it holds (SO-6)", "[hostobject][gc]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	WASM::Store& store = runtime->store();

	const std::filesystem::path path =
			std::filesystem::path(WASM_TEST_DIR) / "host_object_probe.wasm";
	auto module = runtime->loadFile(path);
	REQUIRE(module != nullptr);
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	const WASM::HostType leaf = WASM::HostType::intern(store, leafStruct());
	const WASM::HostType holder = WASM::HostType::intern(store, holderStruct());
	REQUIRE(holder.isInterned());
	// `$holder` is the module's second declared type; its reference field is the
	// pointer-sized field at 16, so the object is 24 bytes.
	CHECK(holder.id() == module->typeId(WASM::LocalTypeIdx{1}));
	CHECK(holder.layout().totalBytes == 24u);
	CHECK(holder.fieldOffset(0) == 16u);
	CHECK(holder.field(0).isReference);

	const std::size_t before = store.gc().liveObjectCount();
	{
		WASM::HostObject parent = WASM::HostObject::createStruct(store, holder);
		WASM::HostObject child = WASM::HostObject::createStruct(store, leaf);
		REQUIRE(parent.isValid());
		REQUIRE(child.isValid());
		child.writeField<std::int32_t>(0, 7);

		// The guest stores the child in the parent, so the field takes a count.
		CHECK_NOTHROW(runtime->callExport<void>(instance, "set_child",
				parent.raw(), child.raw()));
		CHECK(runtime->callExport<std::int32_t>(instance, "has_child", parent.raw()) == 1);

		// The host drops its own child root; the field still holds it.
		child.reset();
		runtime->safepoint();
		CHECK(store.gc().liveObjectCount() == before + 2);
	}
	// Releasing the parent queues it; the walk drops the child and recycles both.
	runtime->safepoint();
	CHECK(store.gc().liveObjectCount() == before);
}

// A cycle of host objects is not freed by the Store's release walk — every link
// keeps a count — but the engine's **trial-deletion** pass still collects it,
// because a guest field store has already registered a candidate. `SO-6` adds a
// walk, never a second collector.
TEST_CASE("a host cycle is collected by the engine's trial-deletion pass", "[hostobject][gc]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	WASM::Store& store = runtime->store();

	const std::filesystem::path path =
			std::filesystem::path(WASM_TEST_DIR) / "host_object_probe.wasm";
	auto module = runtime->loadFile(path);
	REQUIRE(module != nullptr);
	WASM::RegistryImportResolver imports;
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	const WASM::HostType holder = WASM::HostType::intern(store, holderStruct());
	REQUIRE(holder.isInterned());

	const std::size_t before = store.gc().liveObjectCount();
	void* firstRaw = nullptr;
	void* secondRaw = nullptr;
	{
		WASM::HostObject first = WASM::HostObject::createStruct(store, holder);
		WASM::HostObject second = WASM::HostObject::createStruct(store, holder);
		REQUIRE(first.isValid());
		REQUIRE(second.isValid());
		firstRaw = first.raw();
		secondRaw = second.raw();
		CHECK(store.gc().liveObjectCount() == before + 2);
		CHECK(store.gc().totalAllocations() == 2u);
		CHECK(store.gc().totalRecycles() == 0u);
		CHECK_NOTHROW(runtime->callExport<void>(instance, "set_child",
				first.raw(), second.raw()));
		CHECK(instance.gcRefCount(secondRaw) == 2u); // host root + first's field
		CHECK_NOTHROW(runtime->callExport<void>(instance, "set_child",
				second.raw(), first.raw()));
		CHECK(instance.gcRefCount(firstRaw) == 2u); // host root + second's field
	}
	// Each object holds one count: the other object's field, so nothing reaches
	// zero and the Store's release queue stays empty.
	CHECK(instance.gcRefCount(firstRaw) == 1u);
	CHECK(instance.gcRefCount(secondRaw) == 1u);
	CHECK(store.gc().liveObjectCount() == before + 2);
	CHECK(store.gc().totalRecycles() == 0u);

	// The safepoint still collects the cycle: trial deletion owns it, and the
	// guest's field store registered the candidate. The Store adds no collector.
	runtime->safepoint();
	CHECK(store.gc().liveObjectCount() == before);
}

namespace {

//! The table the handle probe's imports resolve through. A `Callable` carries a
//! plain C function pointer — no closure — so the fixture's two imports find the
//! live table here (docs/proposals/SYNTHETIC_OBJECTS.md A.14.4).
WASM::HostHandleTable* g_handleTable = nullptr;

//! Binds `g_handleTable` for the lifetime of one test case.
struct TableBinding {
	explicit TableBinding(WASM::HostHandleTable& table) noexcept
	{
		g_handleTable = &table;
	}
	~TableBinding() { g_handleTable = nullptr; }
	TableBinding(const TableBinding&) = delete;
	TableBinding& operator=(const TableBinding&) = delete;
};

//! `env.handle_get`: field 0 of the object behind the slot, or a trap.
//!
//! The first parameter is the engine's host-import context (a `void*` the
//! backend passes to every native import); this fixture resolves through the
//! file-scope table instead, exactly like the register-ABI tests do.
std::int32_t probeHandleGet(void* context, std::int64_t value)
{
	(void)context;
	WASM::HostObject* object = g_handleTable == nullptr
			? nullptr
			: g_handleTable->resolve(
					  WASM::HostHandle{static_cast<std::uint64_t>(value)});
	if (object == nullptr) {
		// A dead handle is a failure of the guest's obligation, not a value it
		// can branch on: report the trap and return a benign word.
		WASM::reportTrap(WASM::TrapKind::Internal, "host handle is not live");
		return 0;
	}
	return object->readField<std::int32_t>(0);
}

//! `env.handle_set`: writes field 1 of the object behind the slot, or a trap.
void probeHandleSet(void* context, std::int64_t value, std::int32_t written)
{
	(void)context;
	WASM::HostObject* object = g_handleTable == nullptr
			? nullptr
			: g_handleTable->resolve(
					  WASM::HostHandle{static_cast<std::uint64_t>(value)});
	if (object == nullptr) {
		WASM::reportTrap(WASM::TrapKind::Internal, "host handle is not live");
		return;
	}
	object->writeField<std::int32_t>(1, written);
}

} // namespace

// `SO-3`'s acceptance case: one object, two spellings. The table roots the
// object, the guest reaches it as `(ref $pair)` and as the `i64` slot, and the
// engine's count is the only count either path moves — the handle is a root,
// never a second object (SOD4).
TEST_CASE("one object is reached by a reference and by a handle (SO-3)",
		"[hostobject][gc][handle]")
{
	auto runtime = WASM::Runtime::create("libjit");
	REQUIRE(runtime != nullptr);
	WASM::Store& store = runtime->store();

	const std::filesystem::path path =
			std::filesystem::path(WASM_TEST_DIR) / "host_handle_probe.wasm";
	auto module = runtime->loadFile(path);
	REQUIRE(module != nullptr);

	WASM::HostHandleTable table(store);
	TableBinding binding(table);
	WASM::RegistryImportResolver imports;
	imports.registerFunction("env", "handle_get",
			WASM::Callable{reinterpret_cast<void*>(&probeHandleGet), nullptr, 0});
	imports.registerFunction("env", "handle_set",
			WASM::Callable{reinterpret_cast<void*>(&probeHandleSet), nullptr, 0});
	WASM::ModuleInstance& instance = runtime->instantiate(*module, imports);

	const WASM::HostType type = WASM::HostType::intern(store, leafStruct());
	REQUIRE(type.isInterned());
	CHECK(type.id() == module->typeId(WASM::LocalTypeIdx{0}));

	const std::size_t before = store.gc().liveObjectCount();
	WASM::HostObject created = WASM::HostObject::createStruct(store, type);
	REQUIRE(created.isValid());
	created.writeField<std::int32_t>(0, 20);
	created.writeField<std::int32_t>(1, 22);

	// Acquiring *moves* the one count the object already owns: no clone, no
	// second object, and `liveObjectCount` does not move.
	WASM::HostHandle handle = table.acquire(std::move(created), type);
	CHECK_FALSE(created.isValid());
	CHECK(handle.index() == 0u);
	CHECK(handle.generation() == 1u);
	CHECK(table.liveCount() == 1u);
	CHECK(store.gc().liveObjectCount() == before + 1);

	void* raw = table.raw(handle);
	REQUIRE(raw != nullptr);
	const std::int64_t wire = static_cast<std::int64_t>(handle.value);

	// Spelling 1: a reference, validated by the guest's own `ref.test`.
	CHECK(runtime->callExport<std::int32_t>(instance, "is_pair", raw) == 1);
	CHECK(runtime->callExport<std::int32_t>(instance, "sum_pair", raw) == 42);

	// Spelling 2: the slot, resolved by the host inside its own import.
	CHECK(runtime->callExport<std::int32_t>(instance, "handle_read", wire) == 20);
	CHECK_NOTHROW(runtime->callExport<void>(instance, "handle_write", wire,
			std::int32_t{30}));
	// The write went through the handle and landed in the object the typed path
	// reads: 20 + 30, not two objects that happen to agree.
	CHECK(runtime->callExport<std::int32_t>(instance, "sum_pair", raw) == 50);
	CHECK(runtime->callExport<std::int32_t>(instance, "handle_read", wire) == 20);

	// The host validates the type on resolve; a handle of another type is not one.
	const WASM::HostType holder = WASM::HostType::intern(store, holderStruct());
	CHECK(table.resolve(handle, holder) == nullptr);
	CHECK(table.resolve(handle, type) != nullptr);

	// The guest roots the object by reference. Releasing the handle now must not
	// recycle it — the guest's count is not the table's.
	CHECK(runtime->callExport<std::int32_t>(instance, "save", raw) == 1);
	CHECK(table.release(handle));
	CHECK(table.liveCount() == 0u);
	runtime->safepoint();
	CHECK(store.gc().liveObjectCount() == before + 1);
	CHECK(runtime->callExport<std::int32_t>(instance, "sum_pair", raw) == 50);

	// The retired handle can no longer reach the object, even though it is alive.
	CHECK(table.raw(handle) == nullptr);
	CHECK(table.resolve(handle) == nullptr);

	// ... and the guest's last reference is what finally releases it.
	CHECK(runtime->callExport<std::int32_t>(instance, "drop_saved") == 1);
	runtime->safepoint();
	CHECK(store.gc().liveObjectCount() == before);

	// A dead handle that reaches the guest's own call path is refused, not
	// resolved to something plausible: the host import traps with the reason.
	try {
		(void)runtime->callExport<std::int32_t>(instance, "handle_read", wire);
		FAIL("a retired handle must not resolve");
	} catch (const std::exception& error) {
		CHECK(std::string(error.what()).find("host handle is not live")
				!= std::string::npos);
	}
}

// The other half of `SO-3`: every way a slot can be wrong is refused, and a
// double release is a no-op rather than a second `refDrop`.
TEST_CASE("a stale, forged or foreign handle is refused (SO-3)",
		"[hostobject][handle]")
{
	WASM::Store store;
	const WASM::HostType type = WASM::HostType::intern(store, leafStruct());
	REQUIRE(type.isInterned());
	WASM::HostHandleTable table(store);

	const std::size_t before = store.gc().liveObjectCount();
	WASM::HostHandle handle =
			table.acquire(WASM::HostObject::createStruct(store, type), type);
	REQUIRE(!handle.isNone());
	REQUIRE(table.resolve(handle) != nullptr);
	CHECK(store.gc().liveObjectCount() == before + 1);

	// The null handle names nothing, and releasing it is not an error either.
	CHECK(table.resolve(WASM::HostHandle{}) == nullptr);
	CHECK(table.raw(WASM::HostHandle{}) == nullptr);
	CHECK_FALSE(table.release(WASM::HostHandle{}));

	// An index past the table, a generation the slot does not carry, and the
	// reserved generation 0 are all dead handles.
	CHECK(table.resolve(WASM::HostHandle::pack(99u, 1u)) == nullptr);
	CHECK(table.resolve(WASM::HostHandle::pack(handle.index(),
				  handle.generation() + 1u)) == nullptr);
	CHECK(table.resolve(WASM::HostHandle::pack(handle.index(), 0u)) == nullptr);

	// Release once: the root goes, the handle dies, and the slot's generation
	// moves on so the *next* occupant cannot be reached through the old handle.
	CHECK(table.release(handle));
	CHECK(table.resolve(handle) == nullptr);
	CHECK_FALSE(table.release(handle));
	// The release queues the object; the drain is a safepoint operation, and this
	// test has no instance, so the Store's own walk stands in for one.
	store.collectHostObjects();
	CHECK(store.gc().liveObjectCount() == before);
	const std::uint32_t retired = handle.generation();

	WASM::HostHandle reused =
			table.acquire(WASM::HostObject::createStruct(store, type), type);
	CHECK(reused.index() == handle.index());
	CHECK(reused.generation() == retired + 1u);
	CHECK(reused != handle);
	CHECK(table.resolve(reused) != nullptr);
	CHECK(table.resolve(handle) == nullptr);
	CHECK(table.slotCount() == 1u);
	CHECK(table.liveCount() == 1u);

	// A foreign Store's object cannot be rooted: the index would mean nothing here.
	WASM::Store other;
	const WASM::HostType foreign = WASM::HostType::intern(other, leafStruct());
	CHECK_THROWS_AS(
			table.acquire(WASM::HostObject::createStruct(other, foreign), foreign),
			std::invalid_argument);
	// Neither can an object whose identity is not the type it would be filed under.
	const WASM::HostType holder = WASM::HostType::intern(store, holderStruct());
	CHECK_THROWS_AS(
			table.acquire(WASM::HostObject::createStruct(store, holder), type),
			std::logic_error);
	// A refused acquire roots nothing and leaks nothing: both temporaries released
	// their counts on the unwind, and the drain proves neither was rooted here.
	store.collectHostObjects();
	CHECK(table.liveCount() == 1u);
	CHECK(store.gc().liveObjectCount() == before + 1);

	// The table is the owner, not a registry with a manual sweep: releasing the
	// last root puts the count back where it started.
	CHECK(table.release(reused));
	store.collectHostObjects();
	CHECK(store.gc().liveObjectCount() == before);
}

// A table is a slot allocator, not a map: an index comes back, a generation moves
// on, and the destructor is the sweep. Every façade above it (the C++ wrapper, the
// Kotlin one, the emitted `_h_fn` thunks) assumes exactly that, so it is pinned
// here rather than inferred (`SO-3`).
TEST_CASE("the handle table reuses slots without resurrecting handles (SO-3)",
		"[hostobject][handle]")
{
	WASM::Store store;
	const WASM::HostType type = WASM::HostType::intern(store, leafStruct());
	REQUIRE(type.isInterned());
	const std::size_t before = store.gc().liveObjectCount();
	{
		WASM::HostHandleTable table(store);
		std::vector<WASM::HostHandle> handles;
		for (int index = 0; index < 8; ++index) {
			handles.push_back(table.acquire(
					WASM::HostObject::createStruct(store, type), type));
		}
		CHECK(table.liveCount() == 8u);
		CHECK(table.slotCount() == 8u);
		// Distinct slots: an allocator that handed one index out twice would pass
		// every single-object test and fail here.
		for (std::size_t index = 0; index < handles.size(); ++index)
			CHECK(handles[index].index() == index);
		CHECK(store.gc().liveObjectCount() == before + 8);

		// Release in reverse; every slot goes to the free list and the drain
		// reclaims every block.
		for (auto handle = handles.rbegin(); handle != handles.rend(); ++handle)
			CHECK(table.release(*handle));
		CHECK(table.liveCount() == 0u);
		CHECK(table.slotCount() == 8u);
		store.collectHostObjects();
		CHECK(store.gc().liveObjectCount() == before);

		// The next eight reuse those slots — no growth — and each carries the next
		// generation, so nothing from the old batch resolves through a live slot.
		std::vector<WASM::HostHandle> again;
		for (int index = 0; index < 8; ++index) {
			again.push_back(table.acquire(
					WASM::HostObject::createStruct(store, type), type));
		}
		CHECK(table.slotCount() == 8u);
		CHECK(table.liveCount() == 8u);
		for (std::size_t index = 0; index < again.size(); ++index) {
			CHECK(again[index].index() == handles[index].index());
			CHECK(again[index].generation() == handles[index].generation() + 1u);
			CHECK(table.resolve(handles[index]) == nullptr);
			CHECK(table.resolve(again[index]) != nullptr);
		}
		// Leaving the scope destroys the table, and with it the eight roots.
	}
	store.collectHostObjects();
	CHECK(store.gc().liveObjectCount() == before);
}
