#ifndef ELVENDIANNESS_HPP
#define ELVENDIANNESS_HPP
#include <cstdint>
#include <array>
#include <algorithm>
namespace Elv {
namespace Util {

/**
 * @brief Returns the sign of a value as an integer.
 *
 * @tparam T The type of the value (must support comparison with 0).
 * @param val The value to determine the sign of.
 * @return int -1 if val < 0, 1 if val > 0, 0 if val == 0.
 */
template <typename T> int sign(T val) {
	return (T(0) < val) - (val < T(0));
}

/**
 * @enum Endian
 * @brief Enumeration of endianness types.
 */
enum class Endian
{
#ifdef _WIN32
	/**
	 * @brief Little Endian (used on Windows platforms by default).
	 */
	Little = 0,
	/**
	 * @brief Big Endian.
	 */
	Big	= 1,
	/**
	 * @brief Native Endian (matches the platform's native endianness, Little on Windows).
	 */
	Native = Little
#else
	/**
	 * @brief Little Endian (value defined by __ORDER_LITTLE_ENDIAN__).
	 */
	Little = __ORDER_LITTLE_ENDIAN__,
	/**
	 * @brief Big Endian (value defined by __ORDER_BIG_ENDIAN__).
	 */
	Big	= __ORDER_BIG_ENDIAN__,
	/**
	 * @brief Native Endian (matches the platform's native endianness, defined by __BYTE_ORDER__).
	 */
	Native = __BYTE_ORDER__
#endif
};

/**
 * @brief Template struct for endianness swapping of various types.
 *
 * @tparam T The type to be swapped (must be a numeric or compatible type).
 */
template <typename T> struct _endianness_swp {
	/**
	 * @brief Performs endianness swap on the provided value.
	 *
	 * @param val Reference to the value to be swapped.
	 */
	inline static void do_swp(T &val) {
		union U {
			T val;
			std::array<std::uint8_t, sizeof(T)> raw;
		} src, dst;

		src.val = val;
		std::reverse_copy(src.raw.begin(), src.raw.end(), dst.raw.begin());
		val = dst.val;
	}
};

// Specializations for types that can utilize more efficient swapping methods

/**
 * @brief Specialization for uint8_t (no swap needed).
 */
template <> struct _endianness_swp<uint8_t> {
	typedef uint8_t T;
	/**
	 * @brief Dummy implementation, as no swap is required for uint8_t.
	 *
	 * @param val (Unused)
	 */
	inline static void do_swp(T &val) {
		(void)val;
	}
};

/**
 * @brief Specialization for uint16_t.
 */
template <> struct _endianness_swp<uint16_t> {
	typedef uint16_t T;
	/**
	 * @brief Swaps endianness of a uint16_t value.
	 *
	 * Utilizes compiler-specific intrinsics if available, falls back to bitwise operations.
	 *
	 * @param val Reference to the uint16_t value to be swapped.
	 */
	inline static void do_swp(T &val) {
#ifdef __GNUC__
		val = __builtin_bswap16(val);
#elif defined(_MSC_VER)
		val = _byteswap_ushort(val);
#else
		val = (val >> 8) | (val << 8);
#endif
	}
};

/**
 * @brief Specialization for uint32_t.
 */
template <> struct _endianness_swp<uint32_t> {
	typedef uint32_t T;
	/**
	 * @brief Swaps endianness of a uint32_t value.
	 *
	 * Utilizes compiler-specific intrinsics if available, falls back to bitwise operations.
	 *
	 * @param val Reference to the uint32_t value to be swapped.
	 */
	inline static void do_swp(T &val) {
#ifdef __GNUC__
		val = __builtin_bswap32(val);
#elif defined(_MSC_VER)
		val = _byteswap_ulong(val);
#else
		uint32_t tmp = ((val << 8) & 0xFF00FF00) | ((val >> 8) & 0x00FF00FF);
		val = (tmp << 16) | (tmp >> 16);
#endif
	}
};

/**
 * @brief Specialization for uint64_t.
 */
template <> struct _endianness_swp<uint64_t> {
	typedef uint64_t T;
	/**
	 * @brief Swaps endianness of a uint64_t value.
	 *
	 * Utilizes compiler-specific intrinsics if available, falls back to bitwise operations.
	 *
	 * @param val Reference to the uint64_t value to be swapped.
	 */
	inline static void do_swp(T &val) {

#ifdef __GNUC__
		val = __builtin_bswap64(val);
#elif defined(_MSC_VER)
		val = _byteswap_uint64(val);
#else
		val = ((val & 0x00000000FFFFFFFFull) << 32) | ((val & 0xFFFFFFFF00000000ull) >> 32);
		val = ((val & 0x0000FFFF0000FFFFull) << 16) | ((val & 0xFFFF0000FFFF0000ull) >> 16);
		val = ((val & 0x00FF00FF00FF00FFull) << 8)  | ((val & 0xFF00FF00FF00FF00ull) >> 8);
#endif
	}
};

// Aliases for signed types and others, utilizing the unsigned specializations

/**
 * @brief Specialization for int8_t (utilizes uint8_t specialization).
 */
template <> struct _endianness_swp<int8_t> {
	typedef int8_t T;
	/**
	 * @brief Swaps endianness of an int8_t value (no-op, as it's equivalent to uint8_t).
	 *
	 * @param val Reference to the int8_t value.
	 */
	inline static void do_swp(T &val) {
		_endianness_swp<uint8_t>::do_swp(*reinterpret_cast<uint8_t*>(&val));
	}
};

/**
 * @brief Specialization for int16_t (utilizes uint16_t specialization).
 */
template <> struct _endianness_swp<int16_t> {
	typedef int16_t T;
	/**
	 * @brief Swaps endianness of an int16_t value.
	 *
	 * @param val Reference to the int16_t value to be swapped.
	 */
	inline static void do_swp(T &val) {
		_endianness_swp<uint16_t>::do_swp(*reinterpret_cast<uint16_t*>(&val));
	}
};

/**
 * @brief Specialization for int32_t (utilizes uint32_t specialization).
 */
template <> struct _endianness_swp<int32_t> {
	typedef int32_t T;
	/**
	 * @brief Swaps endianness of an int32_t value.
	 *
	 * @param val Reference to the int32_t value to be swapped.
	 */
	inline static void do_swp(T &val) {
		_endianness_swp<uint32_t>::do_swp(*reinterpret_cast<uint32_t*>(&val));
	}
};

/**
 * @brief Specialization for int64_t (utilizes uint64_t specialization).
 */
template <> struct _endianness_swp<int64_t> {
	typedef int64_t T;
	/**
	 * @brief Swaps endianness of an int64_t value.
	 *
	 * @param val Reference to the int64_t value to be swapped.
	 */
	inline static void do_swp(T &val) {
		_endianness_swp<uint64_t>::do_swp(*reinterpret_cast<uint64_t*>(&val));
	}
};

/**
 * @brief Specialization for float (utilizes uint32_t specialization).
 */
template <> struct _endianness_swp<float> {
	typedef float T;
	/**
	 * @brief Swaps endianness of a float value.
	 *
	 * @pre sizeof(float) must equal sizeof(uint32_t).
	 *
	 * @param val Reference to the float value to be swapped.
	 */
	inline static void do_swp(T &val) {
		static_assert (sizeof(float) == sizeof(uint32_t),"Float isn't 32-bit! This means that this kind of system isn't supported!");
		_endianness_swp<uint32_t>::do_swp(*reinterpret_cast<uint32_t*>(&val));
	}
};

/**
 * @brief Specialization for double (utilizes uint64_t specialization).
 */
template <> struct _endianness_swp<double> {
	typedef double T;
	/**
	 * @brief Swaps endianness of a double value.
	 *
	 * @pre sizeof(double) must equal sizeof(uint64_t).
	 *
	 * @param val Reference to the double value to be swapped.
	 */
	inline static void do_swp(T &val) {
		static_assert (sizeof(double) == sizeof(uint64_t),"Double isn't 64-bit! This means that this kind of system isn't supported!");
		_endianness_swp<uint64_t>::do_swp(*reinterpret_cast<uint64_t*>(&val));
	}
};

/**
 * @brief Specialization for char16_t (utilizes uint16_t specialization).
 */
template <> struct _endianness_swp<char16_t> {
	typedef char16_t T;
	/**
	 * @brief Swaps endianness of a char16_t value.
	 *
	 * @pre sizeof(char16_t) must equal sizeof(uint16_t).
	 *
	 * @param val Reference to the char16_t value to be swapped.
	 */
	inline static void do_swp(T &val) {
		static_assert (sizeof(char16_t) == sizeof(uint16_t),"Char16 isn't 16-bit! This means that this kind of system isn't supported!");
		_endianness_swp<uint16_t>::do_swp(*reinterpret_cast<uint16_t*>(&val));
	}
};

/**
 * @brief Specialization for char32_t (utilizes uint32_t specialization).
 */
template <> struct _endianness_swp<char32_t> {
	typedef char32_t T;
	/**
	 * @brief Swaps endianness of a char32_t value.
	 *
	 * @pre sizeof(char32_t) must equal sizeof(uint32_t).
	 *
	 * @param val Reference to the char32_t value to be swapped.
	 */
	inline static void do_swp(T &val) {
		static_assert (sizeof(char32_t) == sizeof(uint32_t),"Char32 isn't 32-bit! This means that this kind of system isn't supported!");
		_endianness_swp<uint32_t>::do_swp(*reinterpret_cast<uint32_t*>(&val));
	}
};

/**
 * @brief Converts the endianness of a value from one format to another.
 *
 * @tparam Endian src The source endianness.
 * @tparam Endian dst The destination endianness.
 * @tparam T The type of the value to convert (must be supported by _endianness_swp).
 * @param val Reference to the value to convert.
 */
template <Endian src, Endian dst, typename T> void convert_endian(T& val) {
	/**
	 * @brief No-op if source and destination endianness are the same.
	 */
	if constexpr(src == dst) (void)val;
	else _endianness_swp<T>::do_swp(val);
};

}
}
#endif // ELVENDIANNESS_HPP
