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
/** \brief Store-owned Unicode code-point sequence, preserving isolated surrogates. */
struct StringObject {
	std::vector<uint32_t> codePoints;
};

//! Opaque views produced by `string.as_wtf8` / `string.as_wtf16`. They are
//! stateless: every operation takes an explicit byte/code-unit position.
/** \brief Opaque immutable view of a Store-owned string for stringview operations. */
struct StringViewObject {
	const StringObject* string;
};

//! `string.as_iter` produces a mutable iterator; `next` advances its position.
/** \brief Mutable code-point iterator over a Store-owned string. */
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

/** \brief Store-owned arena and reference counter for strings, views, and iterators. */
class StringStore
{
public:
	/** \brief Associates this string arena with its owning Store. \param owner Store that owns the arena and allocation budget; must outlive it. */
	explicit StringStore(Store& owner) : owner_(owner) {}

	/** \brief Returns the Store that owns this string arena. \return Reference to the owner Store. */
	Store& owner() const { return owner_; }
	//! May trap with TrapKind::OutOfMemory when the allocation budget is
	//! exhausted. The returned pointer is stable until it is released.
	/** \brief Creates a string object from Unicode code points. \param codePoints Code-point sequence to store. \return Stable pointer to a new unowned string object. \throws OutOfMemory if the Store budget is exhausted. */
	StringObject* create(std::vector<uint32_t> codePoints);
	/** \brief Creates a WTF-8/WTF-16 view over a string. \param string Store-owned source string. \return Stable pointer to a new unowned view. */
	StringViewObject* createView(const StringObject* string);
	/** \brief Creates an iterator positioned at the start of a string. \param string Store-owned source string. \return Stable pointer to a new unowned iterator. */
	StringIteratorObject* createIterator(const StringObject* string);

	//! \par Reference counting (docs/proposals/STRINGREF.md STR-8)
	//! Retain/release a string, view or iterator by address. `false` means the
	//! address is not owned by this Store. A release that reaches zero frees the
	//! object (a view/iterator also releases the string it borrowed).
	/** \brief Increments the reference count of a Store-owned string/view/iterator. \param ref Candidate object pointer. \return true if this Store owns the object and retained it. */
	bool retain(const void* ref);
	/** \brief Releases one reference to a Store-owned string/view/iterator. \param ref Candidate object pointer. \return true if this Store owned the object and released it. */
	bool release(const void* ref);
	/** \brief Tests whether a pointer belongs to this StringStore. \param ref Candidate object pointer. \return true for a string, view, or iterator owned here. */
	bool owns(const void* ref) const;

	//! \par Embedder bridge (docs/proposals/STRINGREF.md STR-7)
	//! Host→string. The input is decoded strictly (WTF-8 permits isolated
	//! surrogates); malformed input or input beyond the implementation limit
	//! returns nullptr and, when `valid` is non-null, sets it to false without
	//! changing the store. Allocation failure still traps with
	//! TrapKind::OutOfMemory. The result is owned (count 1); the host must call
	//! `release` when it no longer needs it.
	/** \brief Decodes strict UTF-8 into a Store-owned string. \param utf8 Input UTF-8 bytes. \param valid Optional output validity flag. \return Owned string, or nullptr for malformed/oversized input. \throws OutOfMemory when allocation fails. */
	StringObject* createFromUtf8(std::string_view utf8, bool* valid = nullptr);
	/** \brief Decodes WTF-8, preserving isolated surrogates. \param wtf8 Input WTF-8 bytes. \param valid Optional output validity flag. \return Owned string, or nullptr for malformed/oversized input. \throws OutOfMemory when allocation fails. */
	StringObject* createFromWtf8(std::string_view wtf8, bool* valid = nullptr);
	/** \brief Decodes UTF-16 code units into a Store-owned string. \param units Input 16-bit code units. \param valid Optional output validity flag. \return Owned string, or nullptr for oversized input. \throws OutOfMemory when allocation fails. */
	StringObject* createFromWtf16(std::span<const uint16_t> units,
								  bool* valid = nullptr);

	//! string→host. `false` means the string cannot be represented in the
	//! requested form (strict UTF-8 with an isolated surrogate); a null string
	//! also returns false.
	/** \brief Encodes a string as UTF-8. \param string Source Store-owned string. \param out Receives encoded bytes. \param lossy Replace isolated surrogates with U+FFFD when true. \return false if strict encoding is impossible or output exceeds limits. */
	bool copyToUtf8(const StringObject* string, std::string& out,
					bool lossy = false) const;
	/** \brief Encodes a string as WTF-8. \param string Source string. \param out Receives encoded bytes. \return true if the encoded size is within the proposal limit. */
	bool copyToWtf8(const StringObject* string, std::vector<uint8_t>& out) const;
	/** \brief Encodes a string as UTF-16 code units. \param string Source string. \param out Receives encoded code units. \return true if the encoded size is within the proposal limit. */
	bool copyToWtf16(const StringObject* string, std::vector<uint16_t>& out) const;

	/** \brief Returns the number of live string/view/iterator objects. \return Number of objects owned by the arena. */
	std::size_t count() const;
private:
	//! Recursive because createView/createIterator retain the string, and
	//! release paths may release a borrowed string. Host threads may create or
	//! drop strings concurrently (SET-2); a StringObject's code-point vector is
	//! immutable after creation, so readers only need the map lock.
	mutable std::recursive_mutex mutex_;

	struct StringEntry {
		StringObject object; //!< Field `object` of StringEntry.
		uint32_t refcount = 0; //!< Field `refcount` of StringEntry.
		uint64_t bytes = 0; //!< Field `bytes` of StringEntry.
	};
	struct ViewEntry {
		StringViewObject object; //!< Field `object` of ViewEntry.
		uint32_t refcount = 0; //!< Field `refcount` of ViewEntry.
		uint64_t bytes = 0; //!< Field `bytes` of ViewEntry.
	};
	/** \\brief Internal lookup entry associating a stable iterator handle with its object. */
struct IteratorEntry {
		StringIteratorObject object;
		uint32_t refcount = 0;
		uint64_t bytes = 0; //!< Field `bytes` of IteratorEntry.
	};

/**
 * \brief Performs the releaseView operation.
 * \param view Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	bool releaseView(const StringViewObject* view);
/**
 * \brief Performs the releaseIterator operation.
 * \param iterator Input value or output destination for this operation.
 * \return The value or status produced by this operation.
 */
	bool releaseIterator(const StringIteratorObject* iterator);

	std::unordered_map<const StringObject*, std::unique_ptr<StringEntry>> objects_; //!< Field `objects_` of type.
	std::unordered_map<const StringViewObject*, std::unique_ptr<ViewEntry>> views_; //!< Field `views_` of type.
	std::unordered_map<const StringIteratorObject*, std::unique_ptr<IteratorEntry>> iterators_; //!< Field `iterators_` of type.

private:
	Store& owner_; //!< Field `owner_` of type.
};

//! \par Codecs
//! Each decoder returns false on malformed input; the caller turns that into
//! the proposal's trap. WTF-8 accepts encoded surrogate code points; UTF-8 does
//! not. WTF-16 reads little-endian 16-bit code units and combines surrogate
//! pairs; isolated surrogates are preserved.
/** \brief Decodes WTF-8 bytes, preserving encoded surrogate code points. \param bytes Input byte buffer. \param size Input length in bytes. \param out Receives decoded code points. \return false for malformed input. */
bool decodeWtf8(const uint8_t* bytes, std::size_t size, std::vector<uint32_t>& out);
/** \brief Decodes strict UTF-8 bytes. \param bytes Input byte buffer. \param size Input length in bytes. \param out Receives decoded Unicode scalar values. \return false for malformed UTF-8 or surrogate encodings. */
bool decodeUtf8(const uint8_t* bytes, std::size_t size, std::vector<uint32_t>& out);
/** \brief Decodes UTF-8, replacing malformed sequences with U+FFFD. \param bytes Input byte buffer. \param size Input length in bytes. \param out Receives decoded code points. */
void decodeUtf8Lossy(const uint8_t* bytes, std::size_t size, std::vector<uint32_t>& out);
/** \brief Decodes little-endian WTF-16 code units, preserving isolated surrogates. \param bytes Input byte buffer. \param codeUnits Number of 16-bit code units. \param out Receives decoded code points. */
void decodeWtf16(const uint8_t* bytes, std::size_t codeUnits, std::vector<uint32_t>& out);

/** \brief Tests whether all code points are Unicode scalar values. \param codePoints Sequence to inspect. \return true when no surrogate code point is present. */
bool isUsvSequence(const std::vector<uint32_t>& codePoints);
//! Return false when the result would exceed the proposal's 31-bit limit or
//! (for UTF-8) when the string contains an isolated surrogate.
/** \brief Measures strict UTF-8 encoded size. \param codePoints Input code points. \param out Receives byte count. \return false for isolated surrogates or a result beyond the 31-bit limit. */
bool measureUtf8(const std::vector<uint32_t>& codePoints, uint32_t& out);
/** \brief Measures WTF-8 encoded size. \param codePoints Input code points. \param out Receives byte count. \return false when the result exceeds the 31-bit limit. */
bool measureWtf8(const std::vector<uint32_t>& codePoints, uint32_t& out);
/** \brief Measures WTF-16 encoded size. \param codePoints Input code points. \param out Receives byte count. \return false when the result exceeds the 31-bit limit. */
bool measureWtf16(const std::vector<uint32_t>& codePoints, uint32_t& out);

//! UTF-8 fails on an isolated surrogate unless `lossy`, in which case U+FFFD is
//! emitted. WTF-8 preserves surrogates. WTF-16 writes little-endian code units.
/** \brief Encodes code points as UTF-8. \param codePoints Source sequence. \param out Receives encoded bytes. \param lossy Replace isolated surrogates with U+FFFD when true. \return false when strict encoding is impossible or exceeds limits. */
bool encodeUtf8(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out,
				bool lossy = false);
/** \brief Encodes code points as WTF-8. \param codePoints Source sequence. \param out Receives encoded bytes. \return false when output exceeds the 31-bit limit. */
bool encodeWtf8(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out);
/** \brief Encodes code points as little-endian WTF-16. \param codePoints Source sequence. \param out Receives encoded bytes. \return false when output exceeds the 31-bit limit. */
bool encodeWtf16(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out);

//! Concatenate, combining a trailing high surrogate and a leading low surrogate
//! into one scalar value as the proposal requires.
/** \brief Concatenates code-point sequences, combining a surrogate pair at the join. \param a Left sequence. \param b Right sequence. \return Newly allocated concatenated code-point sequence. */
std::vector<uint32_t> concatCodePoints(const std::vector<uint32_t>& a,
									   const std::vector<uint32_t>& b);

//! \par stringview helpers (docs/proposals/STRINGREF.md STR-4)
//! Positions use the proposal's "position treatment": an out-of-range position
//! is clamped, and a position inside a codepoint advances to the next boundary.
//! A negative `bytes`/`units`/`count` means "to the end" (or "to the beginning"
//! for `rewind`), matching the proposal's examples.
/** \brief Returns the WTF-8 encoded byte length. \param codePoints Sequence to measure. \return Encoded byte count. */
uint32_t wtf8Length(const std::vector<uint32_t>& codePoints);

//! `wtf8Advance` returns false when the result would exceed 2^31-1 (a trap).
/** \brief Advances a WTF-8 position by a byte count to the next code-point boundary. \param codePoints Source string. \param pos Starting byte position. \param bytes Number of bytes to advance; negative means to the end. \param nextPos Receives the resulting code-point boundary position. \return false when the resulting position exceeds the 31-bit limit. */
bool wtf8Advance(const std::vector<uint32_t>& codePoints, int32_t pos,
				 int32_t bytes, uint32_t& nextPos);

/** \brief Encoding policy used by wtf8Encode(). */
enum class Wtf8Encoding {
	Utf8, //!< Enum member `Utf8`.
	LossyUtf8, //!< Enum member `LossyUtf8`.
	Wtf8 //!< Enum member `Wtf8`.
};
/** \brief Encodes a WTF-8-positioned slice using the selected encoding policy. \param codePoints Source sequence. \param pos Starting byte position. \param bytes Number of bytes to encode; negative means to the end. \param encoding UTF-8/WTF-8 conversion policy. \param nextPos Receives the next position after the encoded slice. \param out Receives encoded bytes. \return false when strict UTF-8 cannot represent the slice or a limit is exceeded. */
bool wtf8Encode(const std::vector<uint32_t>& codePoints, int32_t pos, int32_t bytes,
				Wtf8Encoding encoding, uint32_t& nextPos, std::vector<uint8_t>& out);
/** \brief Extracts a code-point slice using WTF-8 byte-position semantics. \param codePoints Source sequence. \param start Starting byte position. \param end Ending byte position. \return Newly allocated code-point slice. */
std::vector<uint32_t> sliceWtf8(const std::vector<uint32_t>& codePoints,
								int32_t start, int32_t end);

/** \brief Reads a WTF-16 code unit at a position. \param codePoints Source sequence. \param pos Code-unit position. \param unit Receives the 16-bit code unit. \return false when the position is outside the sequence. */
bool wtf16CodeUnitAt(const std::vector<uint32_t>& codePoints, int32_t pos,
					 uint32_t& unit);
/** \brief Encodes a range of code points into little-endian WTF-16 bytes. \param codePoints Source sequence. \param pos Starting code-unit position. \param units Number of code units; negative means to the end. \param out Receives encoded bytes. \return Number of code units written. */
uint32_t wtf16Encode(const std::vector<uint32_t>& codePoints, int32_t pos,
					 int32_t units, std::vector<uint8_t>& out);
/** \brief Extracts a code-point slice using WTF-16 code-unit positions. \param codePoints Source sequence. \param start Starting code-unit position. \param end Ending code-unit position. \return Newly allocated code-point slice. */
std::vector<uint32_t> sliceWtf16(const std::vector<uint32_t>& codePoints,
								 int32_t start, int32_t end);

/** \brief Extracts a code-point-indexed slice. \param codePoints Source sequence. \param start Starting code-point index. \param count Number of code points; negative means to the end. \return Newly allocated code-point slice. */
std::vector<uint32_t> sliceCodePoints(const std::vector<uint32_t>& codePoints,
									  int32_t start, int32_t count);

} // namespace WASM

WASMJIT_VISIBLE_END

#endif // WASMSTRING_HPP
