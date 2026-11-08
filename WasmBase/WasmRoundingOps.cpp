#include "WasmRoundingOps.hpp"

#include <cmath>
#include <limits>

namespace {

std::uint32_t roundingMode(std::uint32_t sub)
{
	return (sub - 0x80u) / 0x14u;
}

float roundToFloat(long double value, std::uint32_t mode)
{
	if (std::isnan(static_cast<double>(value)))
		return std::numeric_limits<float>::quiet_NaN();
	float result = static_cast<float>(value);
	if (std::isinf(result))
		return result;
	const long double back = static_cast<long double>(result);
	switch (mode) {
	case 0:
		if (back < value)
			result = std::nextafter(result, std::numeric_limits<float>::infinity());
		break;
	case 1:
		if (back > value)
			result = std::nextafter(result, -std::numeric_limits<float>::infinity());
		break;
	default:
		if (value >= 0.0L) {
			if (back > value)
				result = std::nextafter(result, -std::numeric_limits<float>::infinity());
		} else if (back < value) {
			result = std::nextafter(result, std::numeric_limits<float>::infinity());
		}
		break;
	}
	return result;
}

double roundToDouble(long double value, std::uint32_t mode)
{
	if (std::isnan(static_cast<double>(value)))
		return std::numeric_limits<double>::quiet_NaN();
	double result = static_cast<double>(value);
	if (std::isinf(result))
		return result;
	const long double back = static_cast<long double>(result);
	switch (mode) {
	case 0:
		if (back < value)
			result = std::nextafter(result, std::numeric_limits<double>::infinity());
		break;
	case 1:
		if (back > value)
			result = std::nextafter(result, -std::numeric_limits<double>::infinity());
		break;
	default:
		if (value >= 0.0L) {
			if (back > value)
				result = std::nextafter(result, -std::numeric_limits<double>::infinity());
		} else if (back < value) {
			result = std::nextafter(result, std::numeric_limits<double>::infinity());
		}
		break;
	}
	return result;
}

} // namespace

extern "C" float wasm_rounding_f32_unary(std::uint32_t sub, float value) noexcept
{
	const std::uint32_t relative = (sub - 0x80u) % 0x14u;
	const long double exact = relative == 0
			? std::sqrt(static_cast<long double>(value))
			: static_cast<long double>(value);
	return roundToFloat(exact, roundingMode(sub));
}

extern "C" float wasm_rounding_f32_binary(std::uint32_t sub, float left,
		float right) noexcept
{
	const std::uint32_t relative = (sub - 0x80u) % 0x14u;
	const long double x = static_cast<long double>(left);
	const long double y = static_cast<long double>(right);
	long double exact = x;
	switch (relative) {
	case 1: exact = x + y; break;
	case 2: exact = x - y; break;
	case 3: exact = x * y; break;
	case 4: exact = x / y; break;
	default: break;
	}
	return roundToFloat(exact, roundingMode(sub));
}

extern "C" double wasm_rounding_f64_unary(std::uint32_t sub, double value) noexcept
{
	const std::uint32_t relative = (sub - 0x80u) % 0x14u;
	const long double exact = relative == 5
			? std::sqrt(static_cast<long double>(value))
			: static_cast<long double>(value);
	return roundToDouble(exact, roundingMode(sub));
}

extern "C" double wasm_rounding_f64_binary(std::uint32_t sub, double left,
		double right) noexcept
{
	const std::uint32_t relative = (sub - 0x80u) % 0x14u;
	const long double x = static_cast<long double>(left);
	const long double y = static_cast<long double>(right);
	long double exact = x;
	switch (relative) {
	case 6: exact = x + y; break;
	case 7: exact = x - y; break;
	case 8: exact = x * y; break;
	case 9: exact = x / y; break;
	default: break;
	}
	return roundToDouble(exact, roundingMode(sub));
}

#define WASM_ROUNDING_CONVERSION(name, type, target, targetType) \
	extern "C" targetType name(std::uint32_t sub, type value) noexcept \
	{ \
		return target(static_cast<long double>(value), roundingMode(sub)); \
	}

WASM_ROUNDING_CONVERSION(wasm_rounding_i32_s_to_f32, std::int32_t, roundToFloat, float)
WASM_ROUNDING_CONVERSION(wasm_rounding_i32_u_to_f32, std::uint32_t, roundToFloat, float)
WASM_ROUNDING_CONVERSION(wasm_rounding_i64_s_to_f32, std::int64_t, roundToFloat, float)
WASM_ROUNDING_CONVERSION(wasm_rounding_i64_u_to_f32, std::uint64_t, roundToFloat, float)
WASM_ROUNDING_CONVERSION(wasm_rounding_f64_to_f32, double, roundToFloat, float)
WASM_ROUNDING_CONVERSION(wasm_rounding_i32_s_to_f64, std::int32_t, roundToDouble, double)
WASM_ROUNDING_CONVERSION(wasm_rounding_i32_u_to_f64, std::uint32_t, roundToDouble, double)
WASM_ROUNDING_CONVERSION(wasm_rounding_i64_s_to_f64, std::int64_t, roundToDouble, double)
WASM_ROUNDING_CONVERSION(wasm_rounding_i64_u_to_f64, std::uint64_t, roundToDouble, double)
WASM_ROUNDING_CONVERSION(wasm_rounding_f32_to_f64, float, roundToDouble, double)
