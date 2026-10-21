#include "WasmString.hpp"
#include "WasmStore.hpp"
#include "WasmTrap.hpp"
#include <utility>

namespace WASM {
namespace {

constexpr uint32_t kUtf8MaxBytes = 0x7FFFFFFFu;   // 2^31 - 1
constexpr uint32_t kWtf16MaxUnits = 0x3FFFFFFFu;  // 2^30 - 1
constexpr uint32_t kReplacement = 0xFFFDu;

bool isSurrogate(uint32_t c) { return c >= 0xD800u && c <= 0xDFFFu; }
bool isHighSurrogate(uint32_t c) { return c >= 0xD800u && c <= 0xDBFFu; }
bool isLowSurrogate(uint32_t c) { return c >= 0xDC00u && c <= 0xDFFFu; }

void appendUtf8(std::vector<uint8_t>& out, uint32_t cp)
{
	if (cp <= 0x7Fu) {
		out.push_back(static_cast<uint8_t>(cp));
	} else if (cp <= 0x7FFu) {
		out.push_back(static_cast<uint8_t>(0xC0u | (cp >> 6)));
		out.push_back(static_cast<uint8_t>(0x80u | (cp & 0x3Fu)));
	} else if (cp <= 0xFFFFu) {
		out.push_back(static_cast<uint8_t>(0xE0u | (cp >> 12)));
		out.push_back(static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu)));
		out.push_back(static_cast<uint8_t>(0x80u | (cp & 0x3Fu)));
	} else {
		out.push_back(static_cast<uint8_t>(0xF0u | (cp >> 18)));
		out.push_back(static_cast<uint8_t>(0x80u | ((cp >> 12) & 0x3Fu)));
		out.push_back(static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu)));
		out.push_back(static_cast<uint8_t>(0x80u | (cp & 0x3Fu)));
	}
}

uint32_t utf8Length(uint32_t cp)
{
	if (cp <= 0x7Fu) return 1;
	if (cp <= 0x7FFu) return 2;
	if (cp <= 0xFFFFu) return 3;
	return 4;
}

// Decode one UTF-8/WTF-8 sequence starting at `index`. Advances `index` on
// success. Rejects overlong forms, truncated sequences, invalid continuation
// bytes, values above U+10FFFF, and (when `allowSurrogates` is false) surrogate
// code points.
bool decodeOne(const uint8_t* bytes, std::size_t size, std::size_t& index,
			   uint32_t& cp, bool allowSurrogates)
{
	const uint8_t b0 = bytes[index];
	if (b0 < 0x80u) {
		cp = b0;
		++index;
		return true;
	}

	std::size_t continuation = 0;
	uint32_t value = 0;
	uint32_t minimum = 0;
	if ((b0 & 0xE0u) == 0xC0u) {
		continuation = 1;
		value = b0 & 0x1Fu;
		minimum = 0x80u;
	} else if ((b0 & 0xF0u) == 0xE0u) {
		continuation = 2;
		value = b0 & 0x0Fu;
		minimum = 0x800u;
	} else if ((b0 & 0xF8u) == 0xF0u) {
		continuation = 3;
		value = b0 & 0x07u;
		minimum = 0x10000u;
	} else {
		return false;
	}

	if (continuation >= size || index >= size - continuation)
		return false;
	for (std::size_t i = 1; i <= continuation; ++i) {
		const uint8_t next = bytes[index + i];
		if ((next & 0xC0u) != 0x80u)
			return false;
		value = (value << 6) | (next & 0x3Fu);
	}
	if (value < minimum || value > 0x10FFFFu)
		return false;
	if (!allowSurrogates && isSurrogate(value))
		return false;

	cp = value;
	index += continuation + 1;
	return true;
}

} // namespace

StringObject* StringStore::create(std::vector<uint32_t> codePoints)
{
	if (codePoints.size() > kMaxStringBytes)
		trap(TrapKind::OutOfMemory, "string exceeds the implementation limit");
	const uint64_t bytes = sizeof(StringObject)
			+ static_cast<uint64_t>(codePoints.size()) * sizeof(uint32_t);
	if (!Store::global().reserveBytes(bytes))
		trap(TrapKind::OutOfMemory, "string allocation exceeds the allocation budget");
	objects_.push_back(StringObject{std::move(codePoints)});
	return &objects_.back();
}

bool decodeWtf8(const uint8_t* bytes, std::size_t size, std::vector<uint32_t>& out)
{
	out.clear();
	out.reserve(size);
	std::size_t index = 0;
	while (index < size) {
		uint32_t cp = 0;
		if (!decodeOne(bytes, size, index, cp, true))
			return false;
		out.push_back(cp);
	}
	return true;
}

bool decodeUtf8(const uint8_t* bytes, std::size_t size, std::vector<uint32_t>& out)
{
	out.clear();
	out.reserve(size);
	std::size_t index = 0;
	while (index < size) {
		uint32_t cp = 0;
		if (!decodeOne(bytes, size, index, cp, false))
			return false;
		out.push_back(cp);
	}
	return true;
}

void decodeUtf8Lossy(const uint8_t* bytes, std::size_t size, std::vector<uint32_t>& out)
{
	out.clear();
	out.reserve(size);
	std::size_t index = 0;
	while (index < size) {
		uint32_t cp = 0;
		const std::size_t before = index;
		if (decodeOne(bytes, size, index, cp, true)) {
			// A surrogate encoded in the input is not a Unicode scalar value;
			// the lossy decoder replaces it, like any other malformed input.
			out.push_back(isSurrogate(cp) ? kReplacement : cp);
		} else {
			// One replacement per unusable byte keeps the decoder progressing;
			// `decodeOne` never advances on failure.
			index = before + 1;
			out.push_back(kReplacement);
		}
	}
}

void decodeWtf16(const uint8_t* bytes, std::size_t codeUnits, std::vector<uint32_t>& out)
{
	out.clear();
	out.reserve(codeUnits);
	for (std::size_t i = 0; i < codeUnits; ++i) {
		const uint32_t unit = static_cast<uint32_t>(bytes[2 * i])
				| (static_cast<uint32_t>(bytes[2 * i + 1]) << 8);
		if (isHighSurrogate(unit) && i + 1 < codeUnits) {
			const uint32_t low = static_cast<uint32_t>(bytes[2 * (i + 1)])
					| (static_cast<uint32_t>(bytes[2 * (i + 1) + 1]) << 8);
			if (isLowSurrogate(low)) {
				out.push_back(0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u));
				++i;
				continue;
			}
		}
		out.push_back(unit);
	}
}

bool isUsvSequence(const std::vector<uint32_t>& codePoints)
{
	for (uint32_t cp : codePoints)
		if (isSurrogate(cp))
			return false;
	return true;
}

bool measureUtf8(const std::vector<uint32_t>& codePoints, uint32_t& out)
{
	uint64_t total = 0;
	for (uint32_t cp : codePoints) {
		if (isSurrogate(cp))
			return false;
		total += utf8Length(cp);
		if (total > kUtf8MaxBytes)
			return false;
	}
	out = static_cast<uint32_t>(total);
	return true;
}

bool measureWtf8(const std::vector<uint32_t>& codePoints, uint32_t& out)
{
	uint64_t total = 0;
	for (uint32_t cp : codePoints) {
		total += utf8Length(cp);
		if (total > kUtf8MaxBytes)
			return false;
	}
	out = static_cast<uint32_t>(total);
	return true;
}

bool measureWtf16(const std::vector<uint32_t>& codePoints, uint32_t& out)
{
	uint64_t total = 0;
	for (uint32_t cp : codePoints) {
		total += (cp > 0xFFFFu) ? 2u : 1u;
		if (total > kWtf16MaxUnits)
			return false;
	}
	out = static_cast<uint32_t>(total);
	return true;
}

bool encodeUtf8(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out, bool lossy)
{
	out.clear();
	out.reserve(codePoints.size() * 3);
	for (uint32_t cp : codePoints) {
		if (isSurrogate(cp)) {
			if (!lossy)
				return false;
			cp = kReplacement;
		}
		appendUtf8(out, cp);
	}
	return true;
}

bool encodeWtf8(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out)
{
	out.clear();
	out.reserve(codePoints.size() * 3);
	for (uint32_t cp : codePoints)
		appendUtf8(out, cp);
	return true;
}

bool encodeWtf16(const std::vector<uint32_t>& codePoints, std::vector<uint8_t>& out)
{
	out.clear();
	out.reserve(codePoints.size() * 2);
	for (uint32_t cp : codePoints) {
		if (cp > 0xFFFFu) {
			const uint32_t v = cp - 0x10000u;
			const uint32_t high = 0xD800u | (v >> 10);
			const uint32_t low = 0xDC00u | (v & 0x3FFu);
			out.push_back(static_cast<uint8_t>(high & 0xFFu));
			out.push_back(static_cast<uint8_t>(high >> 8));
			out.push_back(static_cast<uint8_t>(low & 0xFFu));
			out.push_back(static_cast<uint8_t>(low >> 8));
		} else {
			out.push_back(static_cast<uint8_t>(cp & 0xFFu));
			out.push_back(static_cast<uint8_t>(cp >> 8));
		}
	}
	return true;
}

std::vector<uint32_t> concatCodePoints(const std::vector<uint32_t>& a,
									   const std::vector<uint32_t>& b)
{
	std::vector<uint32_t> out;
	out.reserve(a.size() + b.size());
	out.insert(out.end(), a.begin(), a.end());
	out.insert(out.end(), b.begin(), b.end());
	if (!a.empty() && !b.empty()
			&& isHighSurrogate(a.back()) && isLowSurrogate(b.front())) {
		const uint32_t combined = 0x10000u
				+ ((a.back() - 0xD800u) << 10) + (b.front() - 0xDC00u);
		out[a.size() - 1] = combined;
		out.erase(out.begin() + static_cast<std::ptrdiff_t>(a.size()));
	}
	return out;
}

} // namespace WASM
