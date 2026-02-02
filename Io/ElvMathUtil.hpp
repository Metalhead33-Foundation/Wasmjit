#ifndef ELVMATHUTIL_HPP
#define ELVMATHUTIL_HPP
#include <type_traits>
namespace Elv {
namespace Util {

/**
 * @brief Calculates the ceiling of the division of two integers.
 *
 * This function performs integer division and returns the smallest integer that is greater than or equal to the result.
 *
 * @tparam T The type of the integers to divide. Must be an integral type (e.g., int, long, size_t, etc.).
 *
 * @param a The dividend.
 * @param b The divisor. Must be non-zero.
 *
 * @return The ceiling of the division of `a` by `b`.
 *
 * @throws None (constexpr function, evaluated at compile-time if possible)
 *
 * @example
 *   @code
 *	 int result1 = div_ceil(10, 2);  // result1 == 5
 *	 int result2 = div_ceil(11, 2);  // result2 == 6
 *	 int result3 = div_ceil(10, 5);  // result3 == 2
 *   @endcode
 *
 * @note This function uses SFINAE (Substitution Failure Is Not An Error) to ensure `T` is an integral type.
 *	   If `T` is not an integral type, the function will not be available for overload resolution.
 */
template<typename T>
constexpr
typename std::enable_if<std::is_integral<T>::value, T>::type
div_ceil(T a, T b) {
	/**
	 * If the remainder of `a` divided by `b` is 0, the division is exact, so return the quotient.
	 * Otherwise, return the quotient plus one to round up to the nearest whole number.
	 */
	return (a % b == 0)? (a / b) : (a / b) + 1;
}

}
}
#endif // ELVMATHUTIL_HPP
