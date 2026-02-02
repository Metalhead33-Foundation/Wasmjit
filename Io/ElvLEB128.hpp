#ifndef ELVLEB128_HPP
#define ELVLEB128_HPP
#include "ElvIoDevice.hpp"
#include "ElvDataStream.hpp"
namespace Elv {
namespace Io {

// ----------------------------------------------------------------------------
// Core Algorithms
// ----------------------------------------------------------------------------

template <typename UInt>
UInt readULEB128(Device& dev) {
	static_assert(std::is_unsigned_v<UInt>);
	UInt result = 0;
	unsigned shift = 0;
	std::uint8_t byte = 0;

	while (true) {
		if (dev.read(&byte, 1, 1) != 1)
			throw std::runtime_error("Unexpected EOF reading ULEB128");

		// Safety Guard: Don't shift if we exceed the type's width
		if (shift < sizeof(UInt) * 8) {
			result |= UInt(byte & 0x7F) << shift;
		} else if (byte & 0x7F) {
			// We have exceeded the type width, but the byte still has data.
			// This implies the number is too big for UInt.
			throw std::overflow_error("ULEB128 overflow");
		}

		if ((byte & 0x80) == 0) break;

		shift += 7;
	}
	return result;
}

template <typename SInt>
SInt readSLEB128(Device& dev) {
	static_assert(std::is_signed_v<SInt>);
	SInt result = 0;
	unsigned shift = 0;
	std::uint8_t byte = 0;
	const unsigned size = sizeof(SInt) * 8;

	while (true) {
		if (dev.read(&byte, 1, 1) != 1)
			throw std::runtime_error("Unexpected EOF reading SLEB128");

		if (shift < size) {
			result |= SInt(byte & 0x7F) << shift;
			shift += 7;
		} else if (byte & 0x7F) {
			// Only throw if data bits are lost; standard allows padding 0s (or 1s for negative)
			// But strictly speaking, if we shift out, we check overflow strictly here:
			throw std::overflow_error("SLEB128 overflow");
		}

		if ((byte & 0x80) == 0) break;
	}

	// Sign extension
	if (shift < size && (byte & 0x40)) {
		// -1 is all 1s. Shifting left creates a mask of 1s in the high bits.
		result |= SInt(-1) << shift;
	}

	return result;
}

// Write functions are generally safer, but your logic was good.
// Just ensuring 'value' shifts logically.
template <typename UInt>
void writeULEB128(Device& dev, UInt value) {
	static_assert(std::is_unsigned_v<UInt>);
	do {
		std::uint8_t byte = value & 0x7F;
		value >>= 7;
		if (value != 0) byte |= 0x80;
		dev.write(&byte, 1, 1);
	} while (value != 0);
}

template <typename SInt>
void writeSLEB128(Device& dev, SInt value) {
	static_assert(std::is_signed_v<SInt>);
	bool more = true;
	while (more) {
		std::uint8_t byte = value & 0x7F;
		// Arithmetic shift is guaranteed for signed types in C++20.
		// Before that, it is implementation defined (but usually arithmetic).
		value >>= 7;

		bool signBit = byte & 0x40; // The sign bit of the *byte*, not the original value

		if ((value == 0 && !signBit) || (value == -1 && signBit)) {
			more = false;
		} else {
			byte |= 0x80;
		}
		dev.write(&byte, 1, 1);
	}
}

// ----------------------------------------------------------------------------
// Wrappers (Hold References!)
// ----------------------------------------------------------------------------

// Wrapper for Unsigned LEB128
template <typename T>
struct ULEB128 {
	T& ref;
	explicit ULEB128(T& v) : ref(v) {}
};

// Wrapper for Signed LEB128
template <typename T>
struct SLEB128 {
	T& ref;
	explicit SLEB128(T& v) : ref(v) {}
};

// ----------------------------------------------------------------------------
// Helper Functions (For syntax: stream >> Leb(var))
// ----------------------------------------------------------------------------

template <typename T> auto Leb(T& val) {
	if constexpr (std::is_signed_v<T>) return SLEB128<T>(val);
	else return ULEB128<T>(val);
}

// ----------------------------------------------------------------------------
// Stream Operators
// ----------------------------------------------------------------------------

// Read ULEB
template <Util::Endian E, typename T>
DataStream<E>& operator>>(DataStream<E>& ds, ULEB128<T> wrapper) {
	wrapper.ref = readULEB128<T>(ds.device);
	return ds;
}

// Write ULEB
template <Util::Endian E, typename T>
DataStream<E>& operator<<(DataStream<E>& ds, ULEB128<T> wrapper) {
	writeULEB128<T>(ds.device, wrapper.ref);
	return ds;
}

// Read SLEB
template <Util::Endian E, typename T>
DataStream<E>& operator>>(DataStream<E>& ds, SLEB128<T> wrapper) {
	wrapper.ref = readSLEB128<T>(ds.device);
	return ds;
}

// Write SLEB
template <Util::Endian E, typename T>
DataStream<E>& operator<<(DataStream<E>& ds, SLEB128<T> wrapper) {
	writeSLEB128<T>(ds.device, wrapper.ref);
	return ds;
}

} // namespace Io
} // namespace Elv
#endif // ELVLEB128_HPP
