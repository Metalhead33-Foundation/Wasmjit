#include "AsmjitContext.hpp"

#include <stdexcept>
#include <string>

#if ASMJIT_TARGET_ARCH_X86 == 0
#error "The provisional AsmJit code-emission probe currently supports x86/x64 hosts only"
#endif

namespace AsmJitBackend {

namespace {

void checkError(asmjit::Error error, const char* operation)
{
	if (error != asmjit::Error::kOk)
		throw std::runtime_error(std::string(operation) + ": "
				+ std::to_string(static_cast<unsigned>(error)));
}

} // namespace

Context::Context() = default;

Context::~Context()
{
	for (auto allocation : laneWiseAllocations)
		runtime.release(allocation);
	for (auto allocation : return42Allocations)
		runtime.release(allocation);
}

const asmjit::Environment& Context::environment() const noexcept
{
	return runtime.environment();
}

Context::Return42Entry Context::compileReturn42()
{
	asmjit::CodeHolder code;
	checkError(code.init(runtime.environment(), runtime.cpu_features()),
			"AsmJit CodeHolder initialisation failed");

	asmjit::x86::Assembler assembler(&code);
	checkError(assembler.mov(asmjit::x86::eax, 42),
			"AsmJit instruction emission failed");
	checkError(assembler.ret(), "AsmJit return emission failed");

	Return42Entry entry = nullptr;
	checkError(runtime.add(&entry, &code),
			"AsmJit executable-memory allocation failed");
	try {
		return42Allocations.push_back(entry);
	} catch (...) {
		runtime.release(entry);
		throw;
	}
	return entry;
}

Context::LaneWiseI32x4Entry Context::compileLaneWiseI32x4(bool multiply)
{
	asmjit::CodeHolder code;
	checkError(code.init(runtime.environment(), runtime.cpu_features()),
			"AsmJit CodeHolder initialisation failed");

	asmjit::x86::Compiler compiler(&code);
	asmjit::FuncNode* function = compiler.add_func(
			asmjit::FuncSignature::build<void, const std::uint32_t*,
					const std::uint32_t*, std::uint32_t*>());
	if (function == nullptr)
		throw std::runtime_error("AsmJit failed to create the lane-wise function");

	asmjit::x86::Gp a = compiler.new_gp_ptr("a");
	asmjit::x86::Gp b = compiler.new_gp_ptr("b");
	asmjit::x86::Gp out = compiler.new_gp_ptr("out");
	function->set_arg(0, a);
	function->set_arg(1, b);
	function->set_arg(2, out);

	for (std::int32_t lane = 0; lane < 4; ++lane) {
		const std::int32_t offset = lane * static_cast<std::int32_t>(sizeof(std::uint32_t));
		asmjit::x86::Gp lhs = compiler.new_gp32();
		asmjit::x86::Gp rhs = compiler.new_gp32();
		checkError(compiler.mov(lhs, asmjit::x86::dword_ptr(a, offset)),
				"AsmJit lane load emission failed");
		checkError(compiler.mov(rhs, asmjit::x86::dword_ptr(b, offset)),
				"AsmJit lane load emission failed");
		checkError(multiply ? compiler.imul(lhs, rhs) : compiler.add(lhs, rhs),
				"AsmJit lane arithmetic emission failed");
		checkError(compiler.mov(asmjit::x86::dword_ptr(out, offset), lhs),
				"AsmJit lane store emission failed");
	}

	checkError(compiler.end_func(), "AsmJit function finalisation failed");
	checkError(compiler.finalize(), "AsmJit compiler finalisation failed");

	LaneWiseI32x4Entry entry = nullptr;
	checkError(runtime.add(&entry, &code),
			"AsmJit executable-memory allocation failed");
	try {
		laneWiseAllocations.push_back(entry);
	} catch (...) {
		runtime.release(entry);
		throw;
	}
	return entry;
}

} // namespace AsmJitBackend
