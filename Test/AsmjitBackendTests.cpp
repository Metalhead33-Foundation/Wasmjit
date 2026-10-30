#include <catch2/catch_all.hpp>

#include "Asmjit/AsmjitContext.hpp"

#include <array>
#include <cstdint>

TEST_CASE("AsmJit emits and executes a native function", "[asmjit]")
{
	AsmJitBackend::Context context;
	const auto entry = context.compileReturn42();

	REQUIRE(entry != nullptr);
	CHECK(entry() == 42);
	CHECK(context.environment().arch() == asmjit::Arch::kX64
			|| context.environment().arch() == asmjit::Arch::kX86);
}

TEST_CASE("AsmJit emits lane-wise i32x4 arithmetic", "[asmjit]")
{
	AsmJitBackend::Context context;
	const auto add = context.compileLaneWiseI32x4(false);
	const auto multiply = context.compileLaneWiseI32x4(true);
	REQUIRE(add != nullptr);
	REQUIRE(multiply != nullptr);

	const std::array<std::uint32_t, 4> a{2, 3, 5, 7};
	const std::array<std::uint32_t, 4> b{11, 13, 17, 19};
	std::array<std::uint32_t, 4> result{};

	add(a.data(), b.data(), result.data());
	CHECK(result == std::array<std::uint32_t, 4>{13, 16, 22, 26});

	multiply(a.data(), b.data(), result.data());
	CHECK(result == std::array<std::uint32_t, 4>{22, 39, 85, 133});
}
