#ifndef WASMSTRING_HPP
#define WASMSTRING_HPP
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace WASM {

// Implementation limits. The proposal permits an embedding to impose limits
// more restrictive than its 2^31-1 byte maximum; keeping one decoded string
// well below the allocation budget means the code-point vector cannot be the
// allocation that takes the host down (docs/SAFETY.md).
constexpr uint32_t kMaxStringBytes = 16u << 20;             // 16 MiB
constexpr uint32_t kMaxStringCodeUnits = kMaxStringBytes / 2;

// A host-owned string: the proposal's sequence of Unicode scalar values and
// isolated surrogates. Code points are stored directly (not bytes), so WTF-8,
// WTF-16 and UTF-8 are derived without losing information
// (docs/proposals/STRINGREF.md STRD3).
struct StringObject {
	std::vector<uint32_t> codePoints;
};

// Store-owned arena for StringObjects. Pointers are stable for the Store's
// lifetime. Strings are host-owned and are not reference-counted: they live
// until Store teardown (docs/proposals/STRINGREF.md STRD3/STRD5).
class StringStore
{
public:
	// May trap with TrapKind::OutOfMemory when the allocation budget is
	// exhausted. The returned pointer is stable until the Store dies.
	StringObject* create(std::vector<uint32_t> codePoints);
	std::size_t count() const { return objects_.size(); }
private:
	std::deque<StringObject> objects_;
};

// ── Codecs ──────────────────────────────────────────────────────────────────
// Each decoder returns false on malformed input; the caller turns that into
// the proposal's trap. WTF-8 accepts encoded surrogate code points; UTF-8 does
// not. WTF-16 reads little-endian 16-bit code units and combines surrogate
// pairs; isolated surrogates are preserved.
bool decodeWtf8(const uint8_t* bytes, std::size_t size, std::vector<uint32_t>& out);
bool decodeUtf8(const uint8_t* bytes, std::size_t size, std::vector<uint32_t>& out);
void decodeUtf8Lossy(const uint8_t* bytes, std::size_t size, std::vector<uint32_t>& out);
void decodeWtf16(const uint8_t* bytes, std::size_t codeUnits, std::vector<uint32_t>& out);

bool isUsvSequence(const std::vector<uint32_t>& codePoints);
// Return false when the result would exceed the proposal's 31-bit limit or
// (for UTF-8) when the string contains an isolated surrogate.
bool measureUtf8(const std::vector<uint32_t>& codePoints, uint32_t& out);
bool measureWtf8(const std::vector<uint32_t>& codePoints, uint32_t& out);
bool measureWtf16(const std::vector<uint32_t>& codePoints, uint32_t& out);

// UTF-8 fails on an isolated surrogate unless `lossy`, in which case U+FFFD is
// emitted. WTF-8 preserves surrogates. WTF-16 writes little-endian code units.
bool encodeUtf8(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out,
				bool lossy = false);
bool encodeWtf8(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out);
bool encodeWtf16(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out);

// Concatenate, combining a trailing high surrogate and a leading low surrogate
// into one scalar value as the proposal requires.
std::vector<uint32_t> concatCodePoints(const std::vector<uint32_t>& a,
									   const std::vector<uint32_t>& b);

} // namespace WASM

#endif // WASMSTRING_HPP
