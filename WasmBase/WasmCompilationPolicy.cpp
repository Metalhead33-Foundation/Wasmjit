#include "WasmCompilationPolicy.hpp"

namespace WASM {

const char* optimizationLevelName(OptimizationLevel level) noexcept
{
	switch (level) {
	case OptimizationLevel::None:
		return "none";
	case OptimizationLevel::Basic:
		return "basic";
	case OptimizationLevel::Full:
		return "full";
	}
	return "basic";
}

const char* compilationStrictnessName(CompilationStrictness strictness) noexcept
{
	switch (strictness) {
	case CompilationStrictness::Strict:
		return "strict";
	case CompilationStrictness::Lax:
		return "lax";
	}
	return "strict";
}

const char* interpreterFallbackName(InterpreterFallback fallback) noexcept
{
	switch (fallback) {
	case InterpreterFallback::Never:
		return "never";
	case InterpreterFallback::FallbackOnly:
		return "fallback-only";
	case InterpreterFallback::Always:
		return "always";
	}
	return "never";
}

bool supportsOptimizationLevel(const BackendCompilationCapabilities& capabilities,
		OptimizationLevel level) noexcept
{
	// The enum is ordered from the least to the most effort, so the comparison
	// is the whole rule: a backend that reaches `Full` reaches everything below.
	return static_cast<std::uint8_t>(level)
			<= static_cast<std::uint8_t>(capabilities.maxLevel);
}

bool supportsCompilationPolicy(const BackendCompilationCapabilities& capabilities,
		const CompilationPolicy& policy) noexcept
{
	if (!supportsOptimizationLevel(capabilities, policy.level))
		return false;
	if (policy.fallback != InterpreterFallback::Never && !capabilities.interprets)
		return false;
	return true;
}

std::optional<CompilationPolicy> nextCompilationAttempt(const CompilationPolicy& failed,
const BackendCompilationCapabilities& capabilities) noexcept
{
if (failed.strictness != CompilationStrictness::Lax)
return std::nullopt;
// `Always` never compiles, and a `FallbackOnly` failure means the backend's
	// own interpreter could not run the function either: nothing left to try.
	if (failed.fallback != InterpreterFallback::Never)
		return std::nullopt;

	CompilationPolicy next = failed;
	// An interpreter is exact; a lower degree is only cheaper to produce. Prefer
	// the exact one when the backend says it has it.
	if (capabilities.interprets) {
		next.fallback = InterpreterFallback::FallbackOnly;
		return next;
	}
	switch (failed.level) {
	case OptimizationLevel::Full:
		next.level = OptimizationLevel::Basic;
		return next;
	case OptimizationLevel::Basic:
		next.level = OptimizationLevel::None;
		return next;
	case OptimizationLevel::None:
		return std::nullopt;
	}
	return std::nullopt;
}

CompileFailure::~CompileFailure() = default;

} // namespace WASM
