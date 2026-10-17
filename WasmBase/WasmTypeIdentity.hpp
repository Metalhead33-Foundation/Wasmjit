#ifndef WASMTYPEIDENTITY_HPP
#define WASMTYPEIDENTITY_HPP
#include <cstddef>
#include <cstdint>
#include <functional>
namespace WASM {

// ── Strong type-identity types (docs/TYPE_IDENTITY.md §6.1) ─────────────────
// These are deliberately distinct struct types, not `uint32_t` aliases, so the
// compiler rejects mixing a module-local index with a process-wide identity.
// They are aggregates with a `.value`, so `TypeId{3}` works and there is no
// implicit conversion from integers.

// "type #n in *this* module": a binary-format `typeidx`. Only meaningful
// together with the `Module` it came from.
struct LocalTypeIdx {
	uint32_t value = 0;
	constexpr bool operator==(const LocalTypeIdx& o) const { return value == o.value; }
	constexpr bool operator!=(const LocalTypeIdx& o) const { return value != o.value; }
	constexpr bool operator<(const LocalTypeIdx& o) const { return value < o.value; }
};

// Process-wide identity of one canonical defined type, assigned by TypeRegistry.
struct TypeId {
	// Sentinel for "no canonical identity": used for natively-registered imports
	// whose type the embedder did not pin to a TypeRegistry entry. Never a real
	// canonical id (the table cannot reach 2^32 entries).
	static constexpr uint32_t kNone = 0xFFFFFFFFu;
	uint32_t value = 0;
	constexpr bool operator==(const TypeId& o) const { return value == o.value; }
	constexpr bool operator!=(const TypeId& o) const { return value != o.value; }
	constexpr bool operator<(const TypeId& o) const { return value < o.value; }
	constexpr bool isNone() const { return value == kNone; }
};

// Identity of an interned `rec` group: a contiguous run of TypeIds.
struct RecGroupId {
	uint32_t value = 0;
	constexpr bool operator==(const RecGroupId& o) const { return value == o.value; }
	constexpr bool operator!=(const RecGroupId& o) const { return value != o.value; }
	constexpr bool operator<(const RecGroupId& o) const { return value < o.value; }
};

// One recursive type group: `count` consecutive types starting at `first`.
// A bare type definition is a group of one member.
struct TypeGroup {
	LocalTypeIdx first;
	uint32_t     count = 0;
};

} // namespace WASM

namespace std {
template <> struct hash<WASM::LocalTypeIdx> {
	size_t operator()(const WASM::LocalTypeIdx& v) const noexcept { return std::hash<uint32_t>{}(v.value); }
};
template <> struct hash<WASM::TypeId> {
	size_t operator()(const WASM::TypeId& v) const noexcept { return std::hash<uint32_t>{}(v.value); }
};
template <> struct hash<WASM::RecGroupId> {
	size_t operator()(const WASM::RecGroupId& v) const noexcept { return std::hash<uint32_t>{}(v.value); }
};
} // namespace std

#endif // WASMTYPEIDENTITY_HPP
