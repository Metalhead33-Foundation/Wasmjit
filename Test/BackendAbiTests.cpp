#include <catch2/catch_all.hpp>

#include "WasmBase/WasmBackendAbi.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

using WASM::AggregateLayout;
using WASM::FieldLayout;
using WASM::ValueKind;

// docs/ABI.md "The result tuple layout" — the rule every backend must produce,
// and the one an embedder sizes a result buffer with.

TEST_CASE("the result tuple layout is the documented C layout", "[backendabi]")
{
	const ValueKind kinds[] = { ValueKind::I32, ValueKind::I64, ValueKind::F64 };
	std::array<FieldLayout, 3> fields{};
	const AggregateLayout layout = WASM::computeAggregateLayout(kinds, fields);

	REQUIRE(layout.size == 24);
	REQUIRE(layout.alignment == 8);
	REQUIRE(layout.fieldCount == 3);
	REQUIRE(layout.fields == fields.data());
	REQUIRE(fields[0].offset == 0);
	REQUIRE(fields[0].size == 4);
	REQUIRE(fields[0].alignment == 4);
	REQUIRE(fields[1].offset == 8);
	REQUIRE(fields[1].size == 8);
	REQUIRE(fields[2].offset == 16);
	REQUIRE(fields[2].size == 8);
}

TEST_CASE("a v128 field is 16-byte aligned and rounds the tuple", "[backendabi]")
{
	const ValueKind kinds[] = { ValueKind::I32, ValueKind::V128 };
	std::array<FieldLayout, 2> fields{};
	const AggregateLayout layout = WASM::computeAggregateLayout(kinds, fields);

	REQUIRE(layout.alignment == 16);
	REQUIRE(fields[0].offset == 0);
	REQUIRE(fields[0].size == 4);
	REQUIRE(fields[1].offset == 16);
	REQUIRE(fields[1].size == 16);
	REQUIRE(fields[1].alignment == 16);
	REQUIRE(layout.size == 32);
}

TEST_CASE("an empty tuple, and too little storage, are safe", "[backendabi]")
{
	// An empty tuple, and too little storage, are safe.
	std::array<FieldLayout, 0> none{};
	const AggregateLayout empty = WASM::computeAggregateLayout(
			std::span<const ValueKind>(), none);
	REQUIRE(empty.size == 0);
	REQUIRE(empty.alignment == 1);
	REQUIRE(empty.fieldCount == 0);

	// More kinds than storage must not write out of bounds; it reports the zero
	// layout instead.
	const ValueKind one[] = { ValueKind::I64 };
	const AggregateLayout tooSmall = WASM::computeAggregateLayout(one, none);
	REQUIRE(tooSmall.size == 0);
	REQUIRE(tooSmall.alignment == 1);
	REQUIRE(tooSmall.fieldCount == 0);
}

TEST_CASE("value kinds map onto the ABI classes", "[backendabi]")
{
	REQUIRE(WASM::valueKindOf(WASM::ValueTypeCode::I32) == ValueKind::I32);
	REQUIRE(WASM::valueKindOf(WASM::ValueTypeCode::I64) == ValueKind::I64);
	REQUIRE(WASM::valueKindOf(WASM::ValueTypeCode::F32) == ValueKind::F32);
	REQUIRE(WASM::valueKindOf(WASM::ValueTypeCode::F64) == ValueKind::F64);
	REQUIRE(WASM::valueKindOf(WASM::ValueTypeCode::V128) == ValueKind::V128);
	REQUIRE(WASM::valueKindOf(WASM::ValueTypeCode::Void) == ValueKind::Void);
	// Every reference type is one pointer-sized word.
	REQUIRE(WASM::valueKindOf(WASM::ValueTypeCode::FuncRef) == ValueKind::Ref);
	REQUIRE(WASM::valueKindOf(WASM::ValueTypeCode::ExternRef) == ValueKind::Ref);
	REQUIRE(WASM::valueKindOf(WASM::ValueTypeCode::AnyRef) == ValueKind::Ref);

	REQUIRE(WASM::valueKindSize(ValueKind::Ref) == sizeof(void*));
	REQUIRE(WASM::valueKindAlignment(ValueKind::V128) == 16);
	REQUIRE(WASM::valueKindSize(ValueKind::Void) == 0);
}
