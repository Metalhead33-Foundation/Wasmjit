#include "WasmString.hpp"
#include "WasmStore.hpp"
#include "WasmTrap.hpp"
#include <algorithm>
#include <limits>
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
	std::scoped_lock lock(mutex_);
	if (codePoints.size() > kMaxStringBytes) {
		reportTrap(TrapKind::OutOfMemory, "string exceeds the implementation limit");
		return nullptr;
	}
	const uint64_t bytes = sizeof(StringObject)
			+ static_cast<uint64_t>(codePoints.size()) * sizeof(uint32_t);
	if (!Store::current().reserveBytes(bytes)) {
		reportTrap(TrapKind::OutOfMemory, "string allocation exceeds the allocation budget");
		return nullptr;
	}
	auto entry = std::make_unique<StringEntry>();
	entry->object.codePoints = std::move(codePoints);
	entry->bytes = bytes;
	StringObject* result = &entry->object;
	objects_.emplace(result, std::move(entry));
	return result;
}

StringViewObject* StringStore::createView(const StringObject* string)
{
	std::scoped_lock lock(mutex_);
	const uint64_t bytes = sizeof(StringViewObject);
	if (!Store::current().reserveBytes(bytes)) {
		reportTrap(TrapKind::OutOfMemory, "string view allocation exceeds the allocation budget");
		return nullptr;
	}
	auto entry = std::make_unique<ViewEntry>();
	entry->object.string = string;
	entry->bytes = bytes;
	StringViewObject* result = &entry->object;
	if (string != nullptr)
		retain(string);
	views_.emplace(result, std::move(entry));
	return result;
}

StringIteratorObject* StringStore::createIterator(const StringObject* string)
{
	std::scoped_lock lock(mutex_);
	const uint64_t bytes = sizeof(StringIteratorObject);
	if (!Store::current().reserveBytes(bytes)) {
		reportTrap(TrapKind::OutOfMemory, "string iterator allocation exceeds the allocation budget");
		return nullptr;
	}
	auto entry = std::make_unique<IteratorEntry>();
	entry->object.string = string;
	entry->bytes = bytes;
	StringIteratorObject* result = &entry->object;
	if (string != nullptr)
		retain(string);
	iterators_.emplace(result, std::move(entry));
	return result;
}

bool StringStore::retain(const void* ref)
{
	std::scoped_lock lock(mutex_);
	if (ref == nullptr)
		return false;
	if (const auto* string = static_cast<const StringObject*>(ref);
			objects_.find(string) != objects_.end()) {
		StringEntry& entry = *objects_.find(string)->second;
		if (entry.refcount != std::numeric_limits<uint32_t>::max())
			++entry.refcount;
		return true;
	}
	if (const auto* view = static_cast<const StringViewObject*>(ref);
			views_.find(view) != views_.end()) {
		ViewEntry& entry = *views_.find(view)->second;
		if (entry.refcount != std::numeric_limits<uint32_t>::max())
			++entry.refcount;
		return true;
	}
	if (const auto* iterator = static_cast<const StringIteratorObject*>(ref);
			iterators_.find(iterator) != iterators_.end()) {
		IteratorEntry& entry = *iterators_.find(iterator)->second;
		if (entry.refcount != std::numeric_limits<uint32_t>::max())
			++entry.refcount;
		return true;
	}
	return false;
}

bool StringStore::releaseView(const StringViewObject* view)
{
	std::scoped_lock lock(mutex_);
	const auto it = views_.find(view);
	if (it == views_.end())
		return false;
	ViewEntry& entry = *it->second;
	// An unowned view (count 0) is host/temporary state; leave it for teardown.
	if (entry.refcount == 0)
		return true;
	if (--entry.refcount != 0)
		return true;
	const StringObject* inner = entry.object.string;
	const uint64_t bytes = entry.bytes;
	views_.erase(it);
	Store::current().releaseBytes(bytes);
	if (inner != nullptr)
		release(inner);
	return true;
}

bool StringStore::releaseIterator(const StringIteratorObject* iterator)
{
	std::scoped_lock lock(mutex_);
	const auto it = iterators_.find(iterator);
	if (it == iterators_.end())
		return false;
	IteratorEntry& entry = *it->second;
	if (entry.refcount == 0)
		return true;
	if (--entry.refcount != 0)
		return true;
	const StringObject* inner = entry.object.string;
	const uint64_t bytes = entry.bytes;
	iterators_.erase(it);
	Store::current().releaseBytes(bytes);
	if (inner != nullptr)
		release(inner);
	return true;
}

bool StringStore::release(const void* ref)
{
	std::scoped_lock lock(mutex_);
	if (ref == nullptr)
		return false;
	const auto* string = static_cast<const StringObject*>(ref);
	const auto stringIt = objects_.find(string);
	if (stringIt != objects_.end()) {
		StringEntry& entry = *stringIt->second;
		if (entry.refcount == 0)
			return true; // unowned host/temporary string: Store teardown owns it
		if (--entry.refcount != 0)
			return true;
		const uint64_t bytes = entry.bytes;
		objects_.erase(stringIt);
		Store::current().releaseBytes(bytes);
		return true;
	}
	if (releaseView(static_cast<const StringViewObject*>(ref)))
		return true;
	if (releaseIterator(static_cast<const StringIteratorObject*>(ref)))
		return true;
	return false;
}

bool StringStore::owns(const void* ref) const
{
	std::scoped_lock lock(mutex_);
	if (ref == nullptr)
		return false;
	return objects_.find(static_cast<const StringObject*>(ref)) != objects_.end()
			|| views_.find(static_cast<const StringViewObject*>(ref)) != views_.end()
			|| iterators_.find(static_cast<const StringIteratorObject*>(ref)) != iterators_.end();
}

// ── Embedder bridge (docs/proposals/STRINGREF.md STR-7) ─────────────────────

StringObject* StringStore::createFromUtf8(std::string_view utf8, bool* valid)
{
	if (valid)
		*valid = false;
	if (utf8.size() > kMaxStringBytes)
		return nullptr;
	std::vector<uint32_t> codePoints;
	if (!decodeUtf8(reinterpret_cast<const uint8_t*>(utf8.data()), utf8.size(),
			codePoints))
		return nullptr;
	if (valid)
		*valid = true;
	StringObject* result = create(std::move(codePoints));
	if (result != nullptr)
		retain(result);
	return result;
}

StringObject* StringStore::createFromWtf8(std::string_view wtf8, bool* valid)
{
	if (valid)
		*valid = false;
	if (wtf8.size() > kMaxStringBytes)
		return nullptr;
	std::vector<uint32_t> codePoints;
	if (!decodeWtf8(reinterpret_cast<const uint8_t*>(wtf8.data()), wtf8.size(),
			codePoints))
		return nullptr;
	if (valid)
		*valid = true;
	StringObject* result = create(std::move(codePoints));
	if (result != nullptr)
		retain(result);
	return result;
}

StringObject* StringStore::createFromWtf16(std::span<const uint16_t> units,
										   bool* valid)
{
	if (valid)
		*valid = false;
	if (units.size() > kMaxStringCodeUnits)
		return nullptr;
	// The codec reads little-endian code units, matching the in-memory
	// representation of WTF-16 in linear memory and GC arrays.
	std::vector<uint8_t> bytes(units.size() * 2);
	for (std::size_t i = 0; i < units.size(); ++i) {
		bytes[2 * i] = static_cast<uint8_t>(units[i] & 0xFFu);
		bytes[2 * i + 1] = static_cast<uint8_t>(units[i] >> 8);
	}
	std::vector<uint32_t> codePoints;
	decodeWtf16(bytes.data(), units.size(), codePoints);
	if (valid)
		*valid = true;
	StringObject* result = create(std::move(codePoints));
	if (result != nullptr)
		retain(result);
	return result;
}

bool StringStore::copyToUtf8(const StringObject* string, std::string& out,
							 bool lossy) const
{
	std::scoped_lock lock(mutex_);
	if (string == nullptr)
		return false;
	std::vector<uint8_t> bytes;
	if (!encodeUtf8(string->codePoints, bytes, lossy))
		return false;
	if (bytes.empty())
		out.clear();
	else
		out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	return true;
}

bool StringStore::copyToWtf8(const StringObject* string,
							 std::vector<uint8_t>& out) const
{
	std::scoped_lock lock(mutex_);
	if (string == nullptr)
		return false;
	std::vector<uint8_t> bytes;
	if (!encodeWtf8(string->codePoints, bytes))
		return false;
	out = std::move(bytes);
	return true;
}

bool StringStore::copyToWtf16(const StringObject* string,
							  std::vector<uint16_t>& out) const
{
	std::scoped_lock lock(mutex_);
	if (string == nullptr)
		return false;
	std::vector<uint8_t> bytes;
	if (!encodeWtf16(string->codePoints, bytes))
		return false;
	out.resize(bytes.size() / 2);
	for (std::size_t i = 0; i < out.size(); ++i) {
		out[i] = static_cast<uint16_t>(bytes[2 * i]
				| (static_cast<uint16_t>(bytes[2 * i + 1]) << 8));
	}
	return true;
}

std::size_t StringStore::count() const
{
	std::scoped_lock lock(mutex_);
	return objects_.size();
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

namespace {

// Index of the first WTF-8 codepoint boundary at or after `offset`.
std::size_t wtf8BoundaryIndex(const std::vector<uint32_t>& codePoints, uint64_t offset)
{
	uint64_t boundary = 0;
	for (std::size_t i = 0; i < codePoints.size(); ++i) {
		if (offset <= boundary)
			return i;
		const uint32_t length = utf8Length(codePoints[i]);
		if (offset < boundary + length)
			return i + 1; // inside this codepoint: advance to the next boundary
		boundary += length;
	}
	return codePoints.size();
}

uint32_t wtf8OffsetForIndex(const std::vector<uint32_t>& codePoints, std::size_t index)
{
	uint32_t offset = 0;
	for (std::size_t i = 0; i < index && i < codePoints.size(); ++i)
		offset += utf8Length(codePoints[i]);
	return offset;
}

uint32_t wtf8LengthUnchecked(const std::vector<uint32_t>& codePoints)
{
	uint32_t total = 0;
	for (uint32_t cp : codePoints)
		total += utf8Length(cp);
	return total;
}

uint32_t wtf16LengthUnchecked(const std::vector<uint32_t>& codePoints)
{
	uint32_t total = 0;
	for (uint32_t cp : codePoints)
		total += (cp > 0xFFFFu) ? 2u : 1u;
	return total;
}

std::vector<uint32_t> sliceByCodePointIndex(const std::vector<uint32_t>& codePoints,
											std::size_t start, std::size_t end)
{
	if (end < start)
		return {};
	return std::vector<uint32_t>(codePoints.begin() + static_cast<std::ptrdiff_t>(start),
								 codePoints.begin() + static_cast<std::ptrdiff_t>(end));
}

} // namespace

uint32_t wtf8Length(const std::vector<uint32_t>& codePoints)
{
	return wtf8LengthUnchecked(codePoints);
}

bool wtf8Advance(const std::vector<uint32_t>& codePoints, int32_t pos,
				 int32_t bytes, uint32_t& nextPos)
{
	const uint32_t total = wtf8LengthUnchecked(codePoints);
	uint32_t start = pos < 0 ? 0u : static_cast<uint32_t>(pos);
	if (start > total)
		start = total;
	const std::size_t startIndex = wtf8BoundaryIndex(codePoints, start);
	const uint32_t startOffset = wtf8OffsetForIndex(codePoints, startIndex);
	if (bytes < 0) {
		nextPos = total;
		return true;
	}
	const uint64_t limit = static_cast<uint64_t>(start) + static_cast<uint32_t>(bytes);
	if (limit > 0x7FFFFFFFu)
		return false;
	uint32_t current = startOffset;
	for (std::size_t i = startIndex; i < codePoints.size(); ++i) {
		const uint32_t next = current + utf8Length(codePoints[i]);
		if (next > limit)
			break;
		current = next;
	}
	nextPos = current;
	return true;
}

bool wtf8Encode(const std::vector<uint32_t>& codePoints, int32_t pos, int32_t bytes,
				Wtf8Encoding encoding, uint32_t& nextPos, std::vector<uint8_t>& out)
{
	const uint32_t total = wtf8LengthUnchecked(codePoints);
	uint32_t start = pos < 0 ? 0u : static_cast<uint32_t>(pos);
	if (start > total)
		start = total;
	const std::size_t startIndex = wtf8BoundaryIndex(codePoints, start);
	uint32_t current = wtf8OffsetForIndex(codePoints, startIndex);
	out.clear();
	uint64_t limit = std::numeric_limits<uint64_t>::max();
	if (bytes >= 0) {
		limit = static_cast<uint64_t>(start) + static_cast<uint32_t>(bytes);
		if (limit > 0x7FFFFFFFu)
			return false;
	}
	for (std::size_t i = startIndex; i < codePoints.size(); ++i) {
		const uint32_t cp = codePoints[i];
		uint32_t encoded = cp;
		if (encoding == Wtf8Encoding::Utf8 && isSurrogate(cp))
			return false;
		if (encoding == Wtf8Encoding::LossyUtf8 && isSurrogate(cp))
			encoded = kReplacement;
		const uint32_t length = utf8Length(encoded);
		if (static_cast<uint64_t>(current) + length > limit)
			break;
		appendUtf8(out, encoded);
		current += length;
	}
	nextPos = current;
	return true;
}

std::vector<uint32_t> sliceWtf8(const std::vector<uint32_t>& codePoints,
								int32_t start, int32_t end)
{
	const uint32_t total = wtf8LengthUnchecked(codePoints);
	uint32_t first = start < 0 ? 0u : static_cast<uint32_t>(start);
	if (first > total)
		first = total;
	uint32_t last = end < 0 ? total : static_cast<uint32_t>(end);
	if (last > total)
		last = total;
	return sliceByCodePointIndex(codePoints,
			wtf8BoundaryIndex(codePoints, first), wtf8BoundaryIndex(codePoints, last));
}

bool wtf16CodeUnitAt(const std::vector<uint32_t>& codePoints, int32_t pos,
					 uint32_t& unit)
{
	if (pos < 0)
		return false;
	const uint32_t target = static_cast<uint32_t>(pos);
	uint32_t index = 0;
	for (uint32_t cp : codePoints) {
		if (cp > 0xFFFFu) {
			const uint32_t value = cp - 0x10000u;
			if (index == target) {
				unit = 0xD800u | (value >> 10);
				return true;
			}
			++index;
			if (index == target) {
				unit = 0xDC00u | (value & 0x3FFu);
				return true;
			}
			++index;
		} else {
			if (index == target) {
				unit = cp;
				return true;
			}
			++index;
		}
	}
	return false;
}

uint32_t wtf16Encode(const std::vector<uint32_t>& codePoints, int32_t pos,
					 int32_t units, std::vector<uint8_t>& out)
{
	const uint32_t total = wtf16LengthUnchecked(codePoints);
	uint32_t start = pos < 0 ? 0u : static_cast<uint32_t>(pos);
	if (start > total)
		start = total;
	uint32_t limit = units < 0 ? total - start : static_cast<uint32_t>(units);
	if (limit > total - start)
		limit = total - start;

	out.clear();
	out.reserve(static_cast<std::size_t>(limit) * 2u);
	uint32_t index = 0;
	uint32_t written = 0;
	const auto emit = [&](uint32_t codeUnit) {
		if (index >= start && written < limit) {
			out.push_back(static_cast<uint8_t>(codeUnit & 0xFFu));
			out.push_back(static_cast<uint8_t>(codeUnit >> 8));
			++written;
		}
		++index;
	};
	for (uint32_t cp : codePoints) {
		if (cp > 0xFFFFu) {
			const uint32_t value = cp - 0x10000u;
			emit(0xD800u | (value >> 10));
			emit(0xDC00u | (value & 0x3FFu));
		} else {
			emit(cp);
		}
	}
	return written;
}

std::vector<uint32_t> sliceWtf16(const std::vector<uint32_t>& codePoints,
								 int32_t start, int32_t end)
{
	const uint32_t total = wtf16LengthUnchecked(codePoints);
	uint32_t first = start < 0 ? 0u : static_cast<uint32_t>(start);
	if (first > total)
		first = total;
	uint32_t last = end < 0 ? total : static_cast<uint32_t>(end);
	if (last > total)
		last = total;
	if (last < first)
		return {};

	// Slicing by code units may cut a surrogate pair; decoding the selected
	// units with `decodeWtf16` turns a lone half into an isolated surrogate and
	// combines a complete pair, which is exactly the string model.
	std::vector<uint8_t> bytes;
	encodeWtf16(codePoints, bytes);
	std::vector<uint32_t> out;
	if (last > first)
		decodeWtf16(bytes.data() + static_cast<std::size_t>(first) * 2u,
					last - first, out);
	return out;
}

std::vector<uint32_t> sliceCodePoints(const std::vector<uint32_t>& codePoints,
									  int32_t start, int32_t count)
{
	std::size_t first = start < 0 ? 0u : static_cast<std::size_t>(start);
	if (first > codePoints.size())
		first = codePoints.size();
	const std::size_t take = count < 0
			? codePoints.size() - first
			: std::min<std::size_t>(static_cast<std::size_t>(count),
									codePoints.size() - first);
	return std::vector<uint32_t>(
			codePoints.begin() + static_cast<std::ptrdiff_t>(first),
			codePoints.begin() + static_cast<std::ptrdiff_t>(first + take));
}

} // namespace WASM
