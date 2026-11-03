#include "wasm_f16.hpp"

#include "WasmOpcode.hpp"
#include "WasmTrap.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace {

// ── binary16 primitives ────────────────────────────────────────────────────
//
// The value representation is explicit: a lane is two little-endian bytes, and
// conversion never depends on the host endianness or on a compiler FP16 type.

uint16_t load16(const void* base, uint32_t lane)
{
	const auto* p = static_cast<const uint8_t*>(base) + lane * 2;
	return static_cast<uint16_t>(p[0])
		| static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

void store16(void* base, uint32_t lane, uint16_t value)
{
	auto* p = static_cast<uint8_t*>(base) + lane * 2;
	p[0] = static_cast<uint8_t>(value & 0xFFu);
	p[1] = static_cast<uint8_t>(value >> 8);
}

uint32_t f32ToBits(float value)
{
	uint32_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

float bitsToF32(uint32_t bits)
{
	float value;
	std::memcpy(&value, &bits, sizeof(value));
	return value;
}

// Round-to-nearest-even, the IEEE default and what the proposal's lowering
// (`vcvtps2ph ..., 0`, Arm `Fcvt` with the default FPCR) uses. NaNs become the
// canonical positive binary16 NaN.
uint16_t f32ToF16(float value)
{
	const uint32_t bits = f32ToBits(value);
	const uint16_t sign = static_cast<uint16_t>((bits >> 16) & 0x8000u);
	const uint32_t exp = (bits >> 23) & 0xFFu;
	uint32_t mant = bits & 0x7FFFFFu;

	if (exp == 0xFFu) {
		if (mant != 0)
			return 0x7E00u; // canonical positive binary16 NaN
		return static_cast<uint16_t>(sign | 0x7C00u);     // infinity
	}

	int32_t newExp = static_cast<int32_t>(exp) - 127 + 15;
	if (newExp >= 31)
		return static_cast<uint16_t>(sign | 0x7C00u);     // overflow to infinity
	if (newExp <= 0) {
		if (newExp < -10)
			return sign;                                  // underflow to zero
		mant |= 0x800000u;
		const uint32_t shift = static_cast<uint32_t>(14 - newExp);
		const uint32_t half = 1u << (shift - 1);
		const uint32_t rounded = mant + half - 1u + ((mant >> shift) & 1u);
		return static_cast<uint16_t>(sign | (rounded >> shift));
	}

	uint32_t rounded = mant + 0x0FFFu + ((mant >> 13) & 1u);
	if ((rounded & 0x800000u) != 0) {
		++newExp;
		rounded = 0;
		if (newExp >= 31)
			return static_cast<uint16_t>(sign | 0x7C00u);
	}
	return static_cast<uint16_t>(sign | (static_cast<uint16_t>(newExp) << 10)
			| static_cast<uint16_t>(rounded >> 13));
}

// Direct f64 -> binary16 conversion for `f16x8.demote_f64x2_zero`. Going
// through f32 first would round twice, which the demote instruction does not
// permit. The structure mirrors `f32ToF16`; the wider fields change the
// rounding shift from 13 to 42.
uint16_t f64ToF16(double value)
{
	uint64_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	const uint16_t sign = static_cast<uint16_t>((bits >> 48) & 0x8000u);
	const uint32_t exp = static_cast<uint32_t>((bits >> 52) & 0x7FFu);
	uint64_t mant = bits & UINT64_C(0xFFFFFFFFFFFFF);

	if (exp == 0x7FFu) {
		if (mant != 0)
			return 0x7E00u;
		return static_cast<uint16_t>(sign | 0x7C00u);
	}

	int32_t newExp = static_cast<int32_t>(exp) - 1023 + 15;
	if (newExp >= 31)
		return static_cast<uint16_t>(sign | 0x7C00u);
	if (newExp <= 0) {
		if (newExp < -10)
			return sign;
		mant |= UINT64_C(0x10000000000000);
		const uint32_t shift = static_cast<uint32_t>(43 - newExp);
		const uint64_t half = UINT64_C(1) << (shift - 1);
		const uint64_t rounded = mant + half - 1u + ((mant >> shift) & 1u);
		return static_cast<uint16_t>(sign | (rounded >> shift));
	}

	uint64_t rounded = mant + UINT64_C(0x1FFFFFFFFFF) + ((mant >> 42) & 1u);
	if ((rounded & UINT64_C(0x10000000000000)) != 0) {
		++newExp;
		rounded = 0;
		if (newExp >= 31)
			return static_cast<uint16_t>(sign | 0x7C00u);
	}
	return static_cast<uint16_t>(sign | (static_cast<uint16_t>(newExp) << 10)
			| static_cast<uint16_t>(rounded >> 42));
}

float f16ToF32(uint16_t value)
{
	const uint32_t sign = static_cast<uint32_t>(value & 0x8000u) << 16;
	const uint32_t exp = (value >> 10) & 0x1Fu;
	uint32_t mant = value & 0x3FFu;
	uint32_t bits;

	if (exp == 0) {
		if (mant == 0) {
			bits = sign;
		} else {
			int32_t shift = -1;
			do {
				++shift;
				mant <<= 1;
			} while ((mant & 0x400u) == 0);
			mant &= 0x3FFu;
			bits = sign | (static_cast<uint32_t>(127 - 15 - shift) << 23)
				| (mant << 13);
		}
	} else if (exp == 0x1Fu) {
		bits = sign | 0x7F800000u
			| (mant != 0 ? (0x400000u | (mant << 13)) : 0u);
	} else {
		bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
	}
	return bitsToF32(bits);
}

// ── wasm f16 lane rules ────────────────────────────────────────────────────

uint16_t f16FromF32(float value) { return f32ToF16(value); }
float f16ToF32Lane(uint16_t value) { return f16ToF32(value); }

uint16_t minBits(uint16_t a, uint16_t b)
{
	const float fa = f16ToF32Lane(a);
	const float fb = f16ToF32Lane(b);
	if (std::isnan(fa) || std::isnan(fb))
		return 0x7E00u;
	if (fa < fb)
		return a;
	if (fb < fa)
		return b;
	return std::signbit(fa) ? a : b; // min prefers -0
}

uint16_t maxBits(uint16_t a, uint16_t b)
{
	const float fa = f16ToF32Lane(a);
	const float fb = f16ToF32Lane(b);
	if (std::isnan(fa) || std::isnan(fb))
		return 0x7E00u;
	if (fa > fb)
		return a;
	if (fb > fa)
		return b;
	return std::signbit(fa) ? b : a; // max prefers +0
}

uint16_t pminBits(uint16_t a, uint16_t b)
{
	const float fa = f16ToF32Lane(a);
	const float fb = f16ToF32Lane(b);
	return (fb < fa) ? b : a;
}

uint16_t pmaxBits(uint16_t a, uint16_t b)
{
	const float fa = f16ToF32Lane(a);
	const float fb = f16ToF32Lane(b);
	return (fa < fb) ? b : a;
}

template <typename TInt>
TInt truncSat(float value)
{
	if (std::isnan(value))
		return 0;
	if (static_cast<double>(value) <= static_cast<double>(std::numeric_limits<TInt>::min()))
		return std::numeric_limits<TInt>::min();
	if (static_cast<double>(value) >= static_cast<double>(std::numeric_limits<TInt>::max()))
		return std::numeric_limits<TInt>::max();
	return static_cast<TInt>(value);
}

template <typename TInt>
bool truncTrap(float value)
{
	if (std::isnan(value)) {
		WASM::reportTrap(WASM::TrapKind::InvalidConversionToInteger);
		return false;
	}
	if (!(static_cast<double>(value)
			> static_cast<double>(std::numeric_limits<TInt>::min()) - 1.0
			&& static_cast<double>(value)
			< static_cast<double>(std::numeric_limits<TInt>::max()) + 1.0)) {
		WASM::reportTrap(WASM::TrapKind::IntegerOverflow);
		return false;
	}
	return true;
}

float unaryFloat(uint32_t op, float x)
{
	switch (op) {
	case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Sqrt):    return std::sqrt(x);
	case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Ceil):    return std::ceil(x);
	case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Floor):   return std::floor(x);
	case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Trunc):   return std::trunc(x);
	case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Nearest): return std::nearbyint(x);
	default:                                                    return x;
	}
}

bool isUnaryOp(uint32_t op)
{
	return (op >= 0x130u && op <= 0x136u) || (op >= 0x145u && op <= 0x14Du);
}

bool isBinaryOp(uint32_t op)
{
	return op >= 0x137u && op <= 0x144u;
}

} // namespace

extern "C" void wasm_f16x8_splat(float x, void* out)
{
	const uint16_t lane = f16FromF32(x);
	for (uint32_t i = 0; i < 8; ++i)
		store16(out, i, lane);
}

extern "C" float wasm_f16x8_extract_lane(const void* a, uint32_t lane)
{
	return f16ToF32Lane(load16(a, lane));
}

extern "C" void wasm_f16x8_replace_lane(const void* a, uint32_t lane, float x, void* out)
{
	std::memcpy(out, a, 16);
	store16(out, lane, f16FromF32(x));
}

extern "C" void wasm_f16x8_unary(uint32_t op, const void* a, void* out)
{
	if (!isUnaryOp(op))
		return;

	switch (op) {
	case static_cast<uint32_t>(WASM::SIMDOpcode::I16x8TruncSatF16x8S):
		for (uint32_t i = 0; i < 8; ++i)
			store16(out, i, static_cast<uint16_t>(truncSat<int16_t>(f16ToF32Lane(load16(a, i)))));
		return;
	case static_cast<uint32_t>(WASM::SIMDOpcode::I16x8TruncSatF16x8U):
		for (uint32_t i = 0; i < 8; ++i)
			store16(out, i, static_cast<uint16_t>(truncSat<uint16_t>(f16ToF32Lane(load16(a, i)))));
		return;
	case static_cast<uint32_t>(WASM::SIMDOpcode::I16x8TruncF16x8S):
		for (uint32_t i = 0; i < 8; ++i) {
			const float x = f16ToF32Lane(load16(a, i));
			const bool ok = truncTrap<int16_t>(x);
			store16(out, i, ok ? static_cast<uint16_t>(static_cast<int16_t>(x)) : 0u);
		}
		return;
	case static_cast<uint32_t>(WASM::SIMDOpcode::I16x8TruncF16x8U):
		for (uint32_t i = 0; i < 8; ++i) {
			const float x = f16ToF32Lane(load16(a, i));
			const bool ok = truncTrap<uint16_t>(x);
			store16(out, i, ok ? static_cast<uint16_t>(x) : 0u);
		}
		return;
	case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8ConvertI16x8S):
		for (uint32_t i = 0; i < 8; ++i)
			store16(out, i, f16FromF32(static_cast<float>(static_cast<int16_t>(load16(a, i)))));
		return;
	case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8ConvertI16x8U):
		for (uint32_t i = 0; i < 8; ++i)
			store16(out, i, f16FromF32(static_cast<float>(load16(a, i))));
		return;
	case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8DemoteF32x4Zero):
		std::memset(out, 0, 16);
		for (uint32_t i = 0; i < 4; ++i) {
			float x;
			std::memcpy(&x, static_cast<const uint8_t*>(a) + i * 4, sizeof(x));
			store16(out, i, f16FromF32(x));
		}
		return;
	case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8DemoteF64x2Zero):
		std::memset(out, 0, 16);
		for (uint32_t i = 0; i < 2; ++i) {
			double x;
			std::memcpy(&x, static_cast<const uint8_t*>(a) + i * 8, sizeof(x));
			store16(out, i, f64ToF16(x));
		}
		return;
	case static_cast<uint32_t>(WASM::SIMDOpcode::F32x4PromoteLowF16x8):
		for (uint32_t i = 0; i < 4; ++i) {
			const float x = f16ToF32Lane(load16(a, i));
			std::memcpy(static_cast<uint8_t*>(out) + i * 4, &x, sizeof(x));
		}
		return;
	default:
		break;
	}

	for (uint32_t i = 0; i < 8; ++i) {
		const uint16_t bits = load16(a, i);
		uint16_t result;
		if (op == static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Abs)) {
			result = static_cast<uint16_t>(bits & 0x7FFFu);
		} else if (op == static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Neg)) {
			result = static_cast<uint16_t>(bits ^ 0x8000u);
		} else {
			result = f16FromF32(unaryFloat(op, f16ToF32Lane(bits)));
		}
		store16(out, i, result);
	}
}

extern "C" void wasm_f16x8_binary(uint32_t op, const void* a, const void* b, void* out)
{
	if (!isBinaryOp(op))
		return;

	for (uint32_t i = 0; i < 8; ++i) {
		const uint16_t ab = load16(a, i);
		const uint16_t bb = load16(b, i);
		const float fa = f16ToF32Lane(ab);
		const float fb = f16ToF32Lane(bb);
		uint16_t result;
		switch (op) {
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Eq):   result = (fa == fb) ? 0xFFFFu : 0u; break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Ne):   result = (fa != fb) ? 0xFFFFu : 0u; break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Lt):   result = (fa <  fb) ? 0xFFFFu : 0u; break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Gt):   result = (fa >  fb) ? 0xFFFFu : 0u; break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Le):   result = (fa <= fb) ? 0xFFFFu : 0u; break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Ge):   result = (fa >= fb) ? 0xFFFFu : 0u; break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Add):  result = f16FromF32(fa + fb); break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Sub):  result = f16FromF32(fa - fb); break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Mul):  result = f16FromF32(fa * fb); break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Div):  result = f16FromF32(fa / fb); break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Min):  result = minBits(ab, bb); break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Max):  result = maxBits(ab, bb); break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8PMin): result = pminBits(ab, bb); break;
		case static_cast<uint32_t>(WASM::SIMDOpcode::F16x8PMax): result = pmaxBits(ab, bb); break;
		default:                                                 result = 0; break;
		}
		store16(out, i, result);
	}
}

extern "C" void wasm_f16x8_ternary(uint32_t op, const void* a, const void* b,
		const void* c, void* out)
{
	if (op != static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Madd)
			&& op != static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Nmadd))
		return;

	for (uint32_t i = 0; i < 8; ++i) {
		const float fa = f16ToF32Lane(load16(a, i));
		const float fb = f16ToF32Lane(load16(b, i));
		const float fc = f16ToF32Lane(load16(c, i));
		const float result = (op == static_cast<uint32_t>(WASM::SIMDOpcode::F16x8Madd))
			? std::fma(fa, fb, fc)
			: std::fma(-fa, fb, fc);
		store16(out, i, f16FromF32(result));
	}
}

extern "C" float wasm_f32_load_f16(const void* src)
{
	const auto* p = static_cast<const uint8_t*>(src);
	const uint16_t bits = static_cast<uint16_t>(p[0])
		| static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
	return f16ToF32(bits);
}

extern "C" void wasm_f32_store_f16(void* dst, float x)
{
	const uint16_t bits = f16FromF32(x);
	auto* p = static_cast<uint8_t*>(dst);
	p[0] = static_cast<uint8_t>(bits & 0xFFu);
	p[1] = static_cast<uint8_t>(bits >> 8);
}

