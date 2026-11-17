#include "AsmjitContext.hpp"
#include "AsmjitEmitter.hpp"

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

void Context::release(void* entry) noexcept
{
	if (entry == nullptr)
		return;
	for (auto it = moduleAllocations.begin(); it != moduleAllocations.end(); ++it) {
		if (*it == entry) {
			moduleAllocations.erase(it);
			runtime.release(entry);
			return;
		}
	}
}

Context::Return42Entry Context::compileReturn42()
{
	asmjit::CodeHolder code;
	checkError(code.init(runtime.environment(), runtime.cpu_features()),
			"AsmJit CodeHolder initialisation failed");

	// The probe asks whether this CPU can execute what this backend emits, so it
	// goes through exactly the path the backend uses — the universal compiler and
	// the seam — rather than a host assembler. `BE-7`: the last host spelling of an
	// instruction left the backend with this.
	asmjit::ujit::BackendCompiler compiler(&code);
	asmjit::ujit::UniCompiler universal(&compiler, runtime.cpu_features(),
			asmjit::CpuHints{});
	AsmJitBackend::Emitter emitter(compiler, universal);
	asmjit::FuncNode* function = universal.add_func(asmjit::FuncSignature::build<int>());
	if (function == nullptr)
		throw std::runtime_error("AsmJit failed to create the return-42 probe");
	asmjit::ujit::Gp answer = compiler.new_gp32("answer");
	emitter.mov(answer, 42);
	checkError(compiler.ret(answer), "AsmJit instruction emission failed");
	checkError(universal.end_func(), "AsmJit return emission failed");
	checkError(compiler.finalize(), "AsmJit compiler finalisation failed");

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

	asmjit::ujit::BackendCompiler compiler(&code);
	asmjit::ujit::UniCompiler universal(&compiler, runtime.cpu_features(),
			asmjit::CpuHints{});
	AsmJitBackend::Emitter emitter(compiler, universal);
	asmjit::FuncNode* function = universal.add_func(
			asmjit::FuncSignature::build<void, const std::uint32_t*,
					const std::uint32_t*, std::uint32_t*>());
	if (function == nullptr)
		throw std::runtime_error("AsmJit failed to create the lane-wise function");

	asmjit::ujit::Gp a = compiler.new_gp_ptr("a");
	asmjit::ujit::Gp b = compiler.new_gp_ptr("b");
	asmjit::ujit::Gp out = compiler.new_gp_ptr("out");
	function->set_arg(0, a);
	function->set_arg(1, b);
	function->set_arg(2, out);

	for (std::int32_t lane = 0; lane < 4; ++lane) {
		const std::int32_t offset = lane * static_cast<std::int32_t>(sizeof(std::uint32_t));
		asmjit::ujit::Gp lhs = compiler.new_gp32();
		asmjit::ujit::Gp rhs = compiler.new_gp32();
		emitter.mov(lhs, asmjit::ujit::mem_ptr(a, offset));
		emitter.mov(rhs, asmjit::ujit::mem_ptr(b, offset));
		if (multiply)
			emitter.imul(lhs, rhs);
		else
			emitter.add(lhs, rhs);
		emitter.mov(asmjit::ujit::mem_ptr(out, offset), lhs);
	}

	checkError(universal.end_func(), "AsmJit function finalisation failed");
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

	asmjit::ujit::BackendCompiler compiler(&code);
	asmjit::ujit::UniCompiler universal(&compiler, runtime.cpu_features(),
			asmjit::CpuHints{});
	AsmJitBackend::Emitter emitter(compiler, universal);
	asmjit::FuncNode* function = universal.add_func(
			asmjit::FuncSignature::build<void, const std::uint32_t*,
					const std::uint32_t*, std::uint32_t*>());
	if (function == nullptr)
		throw std::runtime_error("AsmJit failed to create the V128 function");

	asmjit::ujit::Gp a = compiler.new_gp_ptr("a");
	asmjit::ujit::Gp b = compiler.new_gp_ptr("b");
	asmjit::ujit::Gp out = compiler.new_gp_ptr("out");
	function->set_arg(0, a);
	function->set_arg(1, b);
	function->set_arg(2, out);

	asmjit::ujit::Vec lhs = compiler.new_vec128("lhs");
	asmjit::ujit::Vec rhs = compiler.new_vec128("rhs");
	emitter.vectorLoad128(lhs, asmjit::ujit::mem_ptr(a));
	emitter.vectorLoad128(rhs, asmjit::ujit::mem_ptr(b));
			emitter.vector3(asmjit::ujit::UniOpVVV::kAddU32, lhs, lhs, rhs);
	emitter.vectorStore128(asmjit::ujit::mem_ptr(out), lhs);

	checkError(universal.end_func(), "AsmJit V128 function finalisation failed");
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

	asmjit::ujit::BackendCompiler compiler(&code);
	asmjit::ujit::UniCompiler universal(&compiler, runtime.cpu_features(),
			asmjit::CpuHints{});
	AsmJitBackend::Emitter emitter(compiler, universal);
	asmjit::FuncNode* function = universal.add_func(
			asmjit::FuncSignature::build<void, const std::uint8_t*,
					const std::uint8_t*, std::uint8_t*>());
	if (function == nullptr)
		throw std::runtime_error("AsmJit failed to create the V128 bitwise function");

	asmjit::ujit::Gp a = compiler.new_gp_ptr("a");
	asmjit::ujit::Gp b = compiler.new_gp_ptr("b");
	asmjit::ujit::Gp out = compiler.new_gp_ptr("out");
	function->set_arg(0, a);
	function->set_arg(1, b);
	function->set_arg(2, out);

	asmjit::ujit::Vec lhs = compiler.new_vec128("lhs");
	asmjit::ujit::Vec rhs = compiler.new_vec128("rhs");
	emitter.vectorLoad128(lhs, asmjit::ujit::mem_ptr(a));
	emitter.vectorLoad128(rhs, asmjit::ujit::mem_ptr(b));

	switch (operation) {
	case V128BitwiseOp::And:
				emitter.vector3(asmjit::ujit::UniOpVVV::kAndU32, lhs, lhs, rhs);
		break;
	case V128BitwiseOp::Or:
				emitter.vector3(asmjit::ujit::UniOpVVV::kOrU32, lhs, lhs, rhs);
		break;
	case V128BitwiseOp::Xor:
				emitter.vector3(asmjit::ujit::UniOpVVV::kXorU32, lhs, lhs, rhs);
		break;
	default:
		throw std::invalid_argument("Unknown AsmJit V128 bitwise operation");
	}

	emitter.vectorStore128(asmjit::ujit::mem_ptr(out), lhs);
	checkError(universal.end_func(), "AsmJit V128 bitwise function finalisation failed");
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

	asmjit::ujit::BackendCompiler compiler(&code);
	asmjit::ujit::UniCompiler universal(&compiler, runtime.cpu_features(),
			asmjit::CpuHints{});
	AsmJitBackend::Emitter emitter(compiler, universal);
	asmjit::FuncNode* function = universal.add_func(
			asmjit::FuncSignature::build<std::uint32_t, const std::uint8_t*>());
	if (function == nullptr)
		throw std::runtime_error("AsmJit failed to create the packed i8 extension function");

	asmjit::ujit::Gp input = compiler.new_gp_ptr("input");
	asmjit::ujit::Gp result = compiler.new_gp32("result");
	function->set_arg(0, input);
	// The load itself carries the extension, which is the same universal path the
	// module compiler uses for a packed struct field.
	if (signExtend)
		emitter.loadS8(result, asmjit::ujit::mem_ptr(input));
	else
		emitter.loadZ8(result, asmjit::ujit::mem_ptr(input));
	checkError(compiler.ret(result), "AsmJit packed i8 return emission failed");
	checkError(universal.end_func(), "AsmJit packed i8 function finalisation failed");
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

	asmjit::ujit::BackendCompiler compiler(&code);
	asmjit::ujit::UniCompiler universal(&compiler, runtime.cpu_features(),
			asmjit::CpuHints{});
	AsmJitBackend::Emitter emitter(compiler, universal);
	asmjit::FuncNode* function = universal.add_func(
			asmjit::FuncSignature::build<std::uint32_t, const std::uint16_t*>());
	if (function == nullptr)
		throw std::runtime_error("AsmJit failed to create the packed i16 extension function");

	asmjit::ujit::Gp input = compiler.new_gp_ptr("input");
	asmjit::ujit::Gp result = compiler.new_gp32("result");
	function->set_arg(0, input);
	if (signExtend)
		emitter.loadS16(result, asmjit::ujit::mem_ptr(input));
	else
		emitter.loadZ16(result, asmjit::ujit::mem_ptr(input));
	checkError(compiler.ret(result), "AsmJit packed i16 return emission failed");
	checkError(universal.end_func(), "AsmJit packed i16 function finalisation failed");
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
