#include <catch2/catch_all.hpp>

#include <cstdint>
#include <stdexcept>
#include <utility>

#include "WasmBase/WasmGcHeader.hpp"
#include "WasmBase/WasmHostObject.hpp"
#include "WasmBase/WasmHostType.hpp"
#include "WasmBase/WasmStore.hpp"

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

// The capability query is the honest half of scaffolding: a host can ask, and a
// test can pin that the answer flips only when SO-1 lands.
TEST_CASE("the state-changing seam reports itself unavailable", "[hostobject]")
{
	CHECK_FALSE(WASM::hostTypeSupportAvailable());

	WASM::Store store;
	CHECK_THROWS_AS(WASM::internHostType(store, mixedStruct()), std::logic_error);
	CHECK_THROWS_AS(WASM::allocateHostStruct(store, WASM::TypeId{0}), std::logic_error);
	CHECK_THROWS_AS(WASM::allocateHostArray(store, WASM::TypeId{0}, 4), std::logic_error);
}

TEST_CASE("HostType describes a type before it is interned", "[hostobject]")
{
	WASM::HostType type(mixedStruct());
	CHECK_FALSE(type.isInterned());
	CHECK(type.id().isNone());
	CHECK(type.layout().totalBytes == 64);
	CHECK(type.fieldOffset(1) == 24);
	CHECK(type.field(2).bytes == 16);
	CHECK_THROWS_AS(type.field(5), std::out_of_range);

	WASM::Store store;
	CHECK_THROWS_AS(WASM::HostType::intern(store, mixedStruct()), std::logic_error);
}

TEST_CASE("HostObject is a safe empty handle until creation works", "[hostobject]")
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
