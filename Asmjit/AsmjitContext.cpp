#include "AsmjitContext.hpp"

#include <stdexcept>
#include <string>

// AsmJit 1.21 spells the target-architecture switch `ASMJIT_ARCH_X86`; the
// older `ASMJIT_TARGET_ARCH_X86` name is no longer defined, so an undefined
// identifier here would silently evaluate to 0 and trip the #error.
#if ASMJIT_ARCH_X86 == 0
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
	for (auto allocation : moduleAllocations)
		runtime.release(allocation);
	for (auto allocation : packedI16Allocations)
		runtime.release(allocation);
	for (auto allocation : packedI8Allocations)
		runtime.release(allocation);
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

const asmjit::CpuFeatures& Context::cpuFeatures() const noexcept
{
	return runtime.cpu_features();
}

void* Context::install(asmjit::CodeHolder& code)
{
	void* entry = nullptr;
	checkError(runtime.add(&entry, &code), "AsmJit executable-memory allocation failed");
	try {
		moduleAllocations.push_back(entry);
	} catch (...) {
		runtime.release(entry);
		throw;
	}
	return entry;
}

std::size_t Context::allocationCheckpoint() const noexcept
{
	return moduleAllocations.size();
}

void Context::releaseAllocationsSince(std::size_t checkpoint) noexcept
{
	if (checkpoint > moduleAllocations.size())
		return;
	while (moduleAllocations.size() > checkpoint) {
		runtime.release(moduleAllocations.back());
		moduleAllocations.pop_back();
	}
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
	asmjit::x86::Vec rhs = compiler.new_vec128("rhs");
	checkError(compiler.emit(asmjit::x86::Inst::kIdMovdqu, lhs, asmjit::x86::ptr(a)),
			"AsmJit V128 first load emission failed");
	checkError(compiler.emit(asmjit::x86::Inst::kIdMovdqu, rhs, asmjit::x86::ptr(b)),
			"AsmJit V128 second load emission failed");
	checkError(compiler.emit(asmjit::x86::Inst::kIdPaddd, lhs, rhs),
			"AsmJit V128 packed addition emission failed");
	checkError(compiler.emit(asmjit::x86::Inst::kIdMovdqu, asmjit::x86::ptr(out), lhs),
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

	asmjit::x86::Vec lhs = compiler.new_vec128("lhs");
	asmjit::x86::Vec rhs = compiler.new_vec128("rhs");
	checkError(compiler.emit(asmjit::x86::Inst::kIdMovdqu, lhs, asmjit::x86::ptr(a)),
			"AsmJit V128 bitwise first load emission failed");
	checkError(compiler.emit(asmjit::x86::Inst::kIdMovdqu, rhs, asmjit::x86::ptr(b)),
			"AsmJit V128 bitwise second load emission failed");

	switch (operation) {
	case V128BitwiseOp::And:
		checkError(compiler.emit(asmjit::x86::Inst::kIdPand, lhs, rhs),
				"AsmJit V128 AND emission failed");
		break;
	case V128BitwiseOp::Or:
		checkError(compiler.emit(asmjit::x86::Inst::kIdPor, lhs, rhs),
				"AsmJit V128 OR emission failed");
		break;
	case V128BitwiseOp::Xor:
		checkError(compiler.emit(asmjit::x86::Inst::kIdPxor, lhs, rhs),
				"AsmJit V128 XOR emission failed");
		break;
	default:
		throw std::invalid_argument("Unknown AsmJit V128 bitwise operation");
	}

	checkError(compiler.emit(asmjit::x86::Inst::kIdMovdqu, asmjit::x86::ptr(out), lhs),
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

Context::PackedI8ExtendEntry Context::compilePackedI8Extend(bool signExtend)
{
	asmjit::CodeHolder code;
	checkError(code.init(runtime.environment(), runtime.cpu_features()),
			"AsmJit CodeHolder initialisation failed");

	asmjit::x86::Compiler compiler(&code);
	asmjit::FuncNode* function = compiler.add_func(
			asmjit::FuncSignature::build<std::uint32_t, const std::uint8_t*>());
	if (function == nullptr)
		throw std::runtime_error("AsmJit failed to create the packed i8 extension function");

	asmjit::x86::Gp input = compiler.new_gp_ptr("input");
	asmjit::x86::Gp narrow = compiler.new_gp8("narrow");
	asmjit::x86::Gp result = compiler.new_gp32("result");
	function->set_arg(0, input);
	checkError(compiler.mov(narrow, asmjit::x86::byte_ptr(input)),
			"AsmJit packed i8 load emission failed");
	checkError(signExtend ? compiler.movsx(result, narrow) : compiler.movzx(result, narrow),
			"AsmJit packed i8 extension emission failed");
	checkError(compiler.ret(result), "AsmJit packed i8 return emission failed");
	checkError(compiler.end_func(), "AsmJit packed i8 function finalisation failed");
	checkError(compiler.finalize(), "AsmJit packed i8 compiler finalisation failed");

	PackedI8ExtendEntry entry = nullptr;
	checkError(runtime.add(&entry, &code),
			"AsmJit packed i8 executable-memory allocation failed");
	try {
		packedI8Allocations.push_back(entry);
	} catch (...) {
		runtime.release(entry);
		throw;
	}
	return entry;
}

Context::PackedI16ExtendEntry Context::compilePackedI16Extend(bool signExtend)
{
	asmjit::CodeHolder code;
	checkError(code.init(runtime.environment(), runtime.cpu_features()),
			"AsmJit CodeHolder initialisation failed");

	asmjit::x86::Compiler compiler(&code);
	asmjit::FuncNode* function = compiler.add_func(
			asmjit::FuncSignature::build<std::uint32_t, const std::uint16_t*>());
	if (function == nullptr)
		throw std::runtime_error("AsmJit failed to create the packed i16 extension function");

	asmjit::x86::Gp input = compiler.new_gp_ptr("input");
	asmjit::x86::Gp narrow = compiler.new_gp16("narrow");
	asmjit::x86::Gp result = compiler.new_gp32("result");
	function->set_arg(0, input);
	checkError(compiler.mov(narrow, asmjit::x86::word_ptr(input)),
			"AsmJit packed i16 load emission failed");
	checkError(signExtend ? compiler.movsx(result, narrow) : compiler.movzx(result, narrow),
			"AsmJit packed i16 extension emission failed");
	checkError(compiler.ret(result), "AsmJit packed i16 return emission failed");
	checkError(compiler.end_func(), "AsmJit packed i16 function finalisation failed");
	checkError(compiler.finalize(), "AsmJit packed i16 compiler finalisation failed");

	PackedI16ExtendEntry entry = nullptr;
	checkError(runtime.add(&entry, &code),
			"AsmJit packed i16 executable-memory allocation failed");
	try {
		packedI16Allocations.push_back(entry);
	} catch (...) {
		runtime.release(entry);
		throw;
	}
	return entry;
}

} // namespace AsmJitBackend
