#include <catch2/catch_all.hpp>

#include "Asmjit/AsmjitContext.hpp"

TEST_CASE("AsmJit emits and executes a native function", "[asmjit]")
{
	AsmJitBackend::Context context;
	const auto entry = context.compileReturn42();

	REQUIRE(entry != nullptr);
	CHECK(entry() == 42);
	CHECK(context.environment().arch() == asmjit::Arch::kX64
			|| context.environment().arch() == asmjit::Arch::kX86);
}
