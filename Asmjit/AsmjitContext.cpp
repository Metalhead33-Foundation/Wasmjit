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
	for (auto allocation : v128BitwiseAllocations)
		runtime.release(allocation);
	for (auto allocation : v128AddAllocations)
		runtime.release(allocation);
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

Context::V128I32x4AddEntry Context::compileV128I32x4Add()
{
	asmjit::CodeHolder code;
	checkError(code.init(runtime.environment(), runtime.cpu_features()),
			"AsmJit CodeHolder initialisation failed");

	asmjit::x86::Compiler compiler(&code);
	asmjit::FuncNode* function = compiler.add_func(
			asmjit::FuncSignature::build<void, const std::uint32_t*,
					const std::uint32_t*, std::uint32_t*>());
	if (function == nullptr)
		throw std::runtime_error("AsmJit failed to create the V128 function");

	asmjit::x86::Gp a = compiler.new_gp_ptr("a");
	asmjit::x86::Gp b = compiler.new_gp_ptr("b");
	asmjit::x86::Gp out = compiler.new_gp_ptr("out");
	function->set_arg(0, a);
	function->set_arg(1, b);
	function->set_arg(2, out);

	asmjit::x86::Vec lhs = compiler.new_vec128("lhs");
	checkError(compiler.movdqu(lhs, asmjit::x86::ptr(a)),
			"AsmJit V128 first load emission failed");
	checkError(compiler.paddd(lhs, asmjit::x86::ptr(b)),
			"AsmJit V128 packed addition emission failed");
	checkError(compiler.movdqu(asmjit::x86::ptr(out), lhs),
			"AsmJit V128 result store emission failed");

	checkError(compiler.end_func(), "AsmJit V128 function finalisation failed");
	checkError(compiler.finalize(), "AsmJit V128 compiler finalisation failed");

	V128I32x4AddEntry entry = nullptr;
	checkError(runtime.add(&entry, &code),
			"AsmJit V128 executable-memory allocation failed");
	try {
		v128AddAllocations.push_back(entry);
	} catch (...) {
		runtime.release(entry);
		throw;
	}
	return entry;
}

Context::V128BitwiseEntry Context::compileV128Bitwise(V128BitwiseOp operation)
{
	asmjit::CodeHolder code;
	checkError(code.init(runtime.environment(), runtime.cpu_features()),
			"AsmJit CodeHolder initialisation failed");

	asmjit::x86::Compiler compiler(&code);
	asmjit::FuncNode* function = compiler.add_func(
			asmjit::FuncSignature::build<void, const std::uint8_t*,
					const std::uint8_t*, std::uint8_t*>());
	if (function == nullptr)
		throw std::runtime_error("AsmJit failed to create the V128 bitwise function");

	asmjit::x86::Gp a = compiler.new_gp_ptr("a");
	asmjit::x86::Gp b = compiler.new_gp_ptr("b");
	asmjit::x86::Gp out = compiler.new_gp_ptr("out");
	function->set_arg(0, a);
	function->set_arg(1, b);
	function->set_arg(2, out);

	asmjit::x86::Vec value = compiler.new_vec128("value");
	checkError(compiler.movdqu(value, asmjit::x86::ptr(a)),
			"AsmJit V128 bitwise first load emission failed");

	switch (operation) {
	case V128BitwiseOp::And:
		checkError(compiler.pand(value, asmjit::x86::ptr(b)),
				"AsmJit V128 AND emission failed");
		break;
	case V128BitwiseOp::Or:
		checkError(compiler.por(value, asmjit::x86::ptr(b)),
				"AsmJit V128 OR emission failed");
		break;
	case V128BitwiseOp::Xor:
		checkError(compiler.pxor(value, asmjit::x86::ptr(b)),
				"AsmJit V128 XOR emission failed");
		break;
	default:
		throw std::invalid_argument("Unknown AsmJit V128 bitwise operation");
	}

	checkError(compiler.movdqu(asmjit::x86::ptr(out), value),
			"AsmJit V128 bitwise result store emission failed");
	checkError(compiler.end_func(), "AsmJit V128 bitwise function finalisation failed");
	checkError(compiler.finalize(), "AsmJit V128 bitwise compiler finalisation failed");

	V128BitwiseEntry entry = nullptr;
	checkError(runtime.add(&entry, &code),
			"AsmJit V128 bitwise executable-memory allocation failed");
	try {
		v128BitwiseAllocations.push_back(entry);
	} catch (...) {
		runtime.release(entry);
		throw;
	}
	return entry;
}

} // namespace AsmJitBackend
