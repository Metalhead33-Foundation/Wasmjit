#ifndef WASMSTRING_HPP
#define WASMSTRING_HPP

#include "WasmApi.hpp"

/*!
  \file WasmString.hpp
  \brief Stringref values and string constants.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace WASM {

//! Implementation limits. The proposal permits an embedding to impose limits
//! more restrictive than its 2^31-1 byte maximum; keeping one decoded string
//! well below the allocation budget means the code-point vector cannot be the
//! allocation that takes the host down (docs/SAFETY.md).
constexpr uint32_t kMaxStringBytes = 16u << 20;             //!< 16 MiB
constexpr uint32_t kMaxStringCodeUnits = kMaxStringBytes / 2;

//! A Store-owned string: the proposal's sequence of Unicode scalar values and
//! isolated surrogates. Code points are stored directly (not bytes), so WTF-8,
//! WTF-16 and UTF-8 are derived without losing information
//! (docs/proposals/STRINGREF.md STRD3).
struct StringObject {
	std::vector<uint32_t> codePoints;
};

//! Opaque views produced by `string.as_wtf8` / `string.as_wtf16`. They are
//! stateless: every operation takes an explicit byte/code-unit position.
struct StringViewObject {
	const StringObject* string;
};

//! `string.as_iter` produces a mutable iterator; `next` advances its position.
struct StringIteratorObject {
	const StringObject* string;
	uint32_t position = 0; //!< code-point index
};

//! Store-owned arena for StringObjects and the views/iterators derived from
//! them. Pointers are stable while the object is retained. A stringref is
//! reference-counted here (docs/proposals/STRINGREF.md STR-8): `create` and
//! `createView`/`createIterator` return an *unowned* object (count 0), exactly
//! like a fresh GC aggregate; the runtime's `refClone`/`refDrop` barriers call
//! `retain`/`release`, and a zero count frees the object and credits its budget.
class Store;

class StringStore
{
public:
	explicit StringStore(Store& owner) : owner_(owner) {}

	Store& owner() const { return owner_; }
	//! May trap with TrapKind::OutOfMemory when the allocation budget is
	//! exhausted. The returned pointer is stable until it is released.
	StringObject* create(std::vector<uint32_t> codePoints);
	StringViewObject* createView(const StringObject* string);
	StringIteratorObject* createIterator(const StringObject* string);

	//! \par Reference counting (docs/proposals/STRINGREF.md STR-8)
	//! Retain/release a string, view or iterator by address. `false` means the
	//! address is not owned by this Store. A release that reaches zero frees the
	//! object (a view/iterator also releases the string it borrowed).
	bool retain(const void* ref);
	bool release(const void* ref);
	bool owns(const void* ref) const;

	//! \par Embedder bridge (docs/proposals/STRINGREF.md STR-7)
	//! Host→string. The input is decoded strictly (WTF-8 permits isolated
	//! surrogates); malformed input or input beyond the implementation limit
	//! returns nullptr and, when `valid` is non-null, sets it to false without
	//! changing the store. Allocation failure still traps with
	//! TrapKind::OutOfMemory. The result is owned (count 1); the host must call
	//! `release` when it no longer needs it.
	StringObject* createFromUtf8(std::string_view utf8, bool* valid = nullptr);
	StringObject* createFromWtf8(std::string_view wtf8, bool* valid = nullptr);
	StringObject* createFromWtf16(std::span<const uint16_t> units,
								  bool* valid = nullptr);

	//! string→host. `false` means the string cannot be represented in the
	//! requested form (strict UTF-8 with an isolated surrogate); a null string
	//! also returns false.
	bool copyToUtf8(const StringObject* string, std::string& out,
					bool lossy = false) const;
	bool copyToWtf8(const StringObject* string, std::vector<uint8_t>& out) const;
	bool copyToWtf16(const StringObject* string, std::vector<uint16_t>& out) const;

	std::size_t count() const;
private:
	//! Recursive because createView/createIterator retain the string, and
	//! release paths may release a borrowed string. Host threads may create or
	//! drop strings concurrently (SET-2); a StringObject's code-point vector is
	//! immutable after creation, so readers only need the map lock.
	mutable std::recursive_mutex mutex_;

	struct StringEntry {
		StringObject object;
		uint32_t refcount = 0;
		uint64_t bytes = 0;
	};
	struct ViewEntry {
		StringViewObject object;
		uint32_t refcount = 0;
		uint64_t bytes = 0;
	};
	struct IteratorEntry {
		StringIteratorObject object;
		uint32_t refcount = 0;
		uint64_t bytes = 0;
	};

	bool releaseView(const StringViewObject* view);
	bool releaseIterator(const StringIteratorObject* iterator);

	std::unordered_map<const StringObject*, std::unique_ptr<StringEntry>> objects_;
	std::unordered_map<const StringViewObject*, std::unique_ptr<ViewEntry>> views_;
	std::unordered_map<const StringIteratorObject*, std::unique_ptr<IteratorEntry>> iterators_;

private:
	Store& owner_;
};

//! \par Codecs
//! Each decoder returns false on malformed input; the caller turns that into
//! the proposal's trap. WTF-8 accepts encoded surrogate code points; UTF-8 does
//! not. WTF-16 reads little-endian 16-bit code units and combines surrogate
//! pairs; isolated surrogates are preserved.
bool decodeWtf8(const uint8_t* bytes, std::size_t size, std::vector<uint32_t>& out);
bool decodeUtf8(const uint8_t* bytes, std::size_t size, std::vector<uint32_t>& out);
void decodeUtf8Lossy(const uint8_t* bytes, std::size_t size, std::vector<uint32_t>& out);
void decodeWtf16(const uint8_t* bytes, std::size_t codeUnits, std::vector<uint32_t>& out);

bool isUsvSequence(const std::vector<uint32_t>& codePoints);
//! Return false when the result would exceed the proposal's 31-bit limit or
//! (for UTF-8) when the string contains an isolated surrogate.
bool measureUtf8(const std::vector<uint32_t>& codePoints, uint32_t& out);
bool measureWtf8(const std::vector<uint32_t>& codePoints, uint32_t& out);
bool measureWtf16(const std::vector<uint32_t>& codePoints, uint32_t& out);

//! UTF-8 fails on an isolated surrogate unless `lossy`, in which case U+FFFD is
//! emitted. WTF-8 preserves surrogates. WTF-16 writes little-endian code units.
bool encodeUtf8(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out,
				bool lossy = false);
bool encodeWtf8(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out);
bool encodeWtf16(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out);

//! Concatenate, combining a trailing high surrogate and a leading low surrogate
//! into one scalar value as the proposal requires.
std::vector<uint32_t> concatCodePoints(const std::vector<uint32_t>& a,
									   const std::vector<uint32_t>& b);

//! \par stringview helpers (docs/proposals/STRINGREF.md STR-4)
//! Positions use the proposal's "position treatment": an out-of-range position
//! is clamped, and a position inside a codepoint advances to the next boundary.
//! A negative `bytes`/`units`/`count` means "to the end" (or "to the beginning"
//! for `rewind`), matching the proposal's examples.
uint32_t wtf8Length(const std::vector<uint32_t>& codePoints);

//! `wtf8Advance` returns false when the result would exceed 2^31-1 (a trap).
bool wtf8Advance(const std::vector<uint32_t>& codePoints, int32_t pos,
				 int32_t bytes, uint32_t& nextPos);

enum class Wtf8Encoding { Utf8, LossyUtf8, Wtf8 };
bool wtf8Encode(const std::vector<uint32_t>& codePoints, int32_t pos, int32_t bytes,
				Wtf8Encoding encoding, uint32_t& nextPos, std::vector<uint8_t>& out);
std::vector<uint32_t> sliceWtf8(const std::vector<uint32_t>& codePoints,
								int32_t start, int32_t end);

bool wtf16CodeUnitAt(const std::vector<uint32_t>& codePoints, int32_t pos,
					 uint32_t& unit);
uint32_t wtf16Encode(const std::vector<uint32_t>& codePoints, int32_t pos,
					 int32_t units, std::vector<uint8_t>& out);
std::vector<uint32_t> sliceWtf16(const std::vector<uint32_t>& codePoints,
								 int32_t start, int32_t end);

std::vector<uint32_t> sliceCodePoints(const std::vector<uint32_t>& codePoints,
									  int32_t start, int32_t count);

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMSTRING_HPP
