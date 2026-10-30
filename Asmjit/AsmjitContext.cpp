#include "AsmjitContext.hpp"

#include <stdexcept>
#include <string>

#if ASMJIT_TARGET_ARCH_X86 == 0
#error "The provisional AsmJit code-emission probe currently supports x86/x64 hosts only"
#endif

namespace AsmJitBackend {

Context::Context() = default;

Context::~Context()
{
	for (auto allocation : allocations)
		runtime.release(allocation);
}

const asmjit::Environment& Context::environment() const noexcept
{
	return runtime.environment();
}

int (*Context::compileReturn42())()
{
	asmjit::CodeHolder code;
	asmjit::Error error = code.init(runtime.environment());
	if (error != asmjit::Error::kOk)
		throw std::runtime_error("AsmJit CodeHolder initialisation failed: "
				+ std::to_string(static_cast<unsigned>(error)));

	asmjit::x86::Assembler assembler(&code);
	error = assembler.mov(asmjit::x86::eax, 42);
	if (error == asmjit::Error::kOk)
		error = assembler.ret();
	if (error != asmjit::Error::kOk)
		throw std::runtime_error("AsmJit instruction emission failed: "
				+ std::to_string(static_cast<unsigned>(error)));

	using Entry = int (*)();
	Entry entry = nullptr;
	error = runtime.add(&entry, &code);
	if (error != asmjit::Error::kOk)
		throw std::runtime_error("AsmJit executable-memory allocation failed: "
				+ std::to_string(static_cast<unsigned>(error)));

	allocations.push_back(entry);
	return entry;
}

} // namespace AsmJitBackend
