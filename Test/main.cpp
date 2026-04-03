#include <iostream>
#include <Elvavena/Io/ElvDataStream.hpp>
#include <Euphemy/Io/EuphFile.hpp>
#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include "WasmBase/WasmModule.hpp"
#include "LibJit/LibJitContext.hpp"
#include "WasmBase/WasmType.hpp"
#include "WasmStub/StubOpcodeDispatcher.hpp"
#include <Euphemy/Config/GlobalConfig.hpp>

using namespace std;
typedef Euph::Io::File RegularFile;
typedef Elv::Io::DataStream<Elv::Util::Endian::Little> WasmStream;
#define _WASMPATH "/home/legacy/helloworld3.wasm"
//#define _WASMPATH "/home/legacy/programok/programkodok/cartridge/cartridge2.wasm"
//#define _WASMPATH "/home/legacy/programok/programkodok/go/app.wasm"
static const char* WASMPATH = _WASMPATH;
Euph::Conf::Configuration GLOBAL_CONFIGURATION;

void printType(jit_type_t type)
{
	const bool isPointer = jit_type_is_pointer(type);
	const bool isTagged = jit_type_is_tagged(type);
	const bool isStruct = jit_type_is_struct(type);
	const bool isFunction = jit_type_is_signature(type);
	if(isPointer) type = jit_type_get_ref(type);
	//if(isFunction) std::cout << "function";
	if(isFunction) {
		std::cout << "function: ";
		jit_type_t returnType = jit_type_get_return(type);
		unsigned paramNum = jit_type_num_params(type);
		printType(returnType);
		std::cout << '(';
		for(unsigned i = 0; i < paramNum; ++i)
		{
			jit_type_t param = jit_type_get_param(type, i);
			printType(param);
			if(i+1 < paramNum) std::cout << ", ";
		}
		std::cout << ')';
	}
	else if(isStruct) std::cout << "struct";

	/*if(type == jit_type_void) std::cout << "void";
	else if(type == jit_type_sbyte) std::cout << "int8_t";
	else if(type == jit_type_ubyte) std::cout << "uint8_t";
	else if(type == jit_type_short) std::cout << "int16_t";
	else if(type == jit_type_ushort) std::cout << "uint16_t";
	else if(type == jit_type_int) std::cout << "int32_t";
	else if(type == jit_type_uint) std::cout << "uint32_t";
	else if(type == jit_type_nint) std::cout << "intptr_t";
	else if(type == jit_type_nuint) std::cout << "uintptr_t";
	else if(type == jit_type_long) std::cout << "int64_t";
	else if(type == jit_type_ulong) std::cout << "uint64_t";
	else if(type == jit_type_float32) std::cout << "float";
	else if(type == jit_type_float64) std::cout << "double";
	else if(type == jit_type_nfloat) std::cout << "bigfloat";
	else if(type == jit_type_void_ptr) std::cout << "void*";*/
	if(jit_type_is_primitive(type)) switch (jit_type_get_kind(type) ) {
		case JIT_TYPE_INVALID:
			std::cout << "invalid";
			break;
		case JIT_TYPE_VOID:
			std::cout << "void";
			break;
		case JIT_TYPE_SBYTE:
			std::cout << "int8_t";
			break;
		case JIT_TYPE_UBYTE:
			std::cout << "uint8_t";
			break;
		case JIT_TYPE_SHORT:
			std::cout << "int16_t";
			break;
		case JIT_TYPE_USHORT:
			std::cout << "uint16_t";
			break;
		case JIT_TYPE_INT:
			std::cout << "int32_t";
			break;
		case JIT_TYPE_UINT:
			std::cout << "uint32_t";
			break;
		case JIT_TYPE_NINT:
			std::cout << "intptr_t";
			break;
		case JIT_TYPE_NUINT:
			std::cout << "uintptr_t";
			break;
		case JIT_TYPE_LONG:
			std::cout << "int64_t";
			break;
		case JIT_TYPE_ULONG:
			std::cout << "uint64_t";
			break;
		case JIT_TYPE_FLOAT32:
			std::cout << "float";
			break;
		case JIT_TYPE_FLOAT64:
			std::cout << "double";
			break;
		case JIT_TYPE_NFLOAT:
			std::cout << "bigfloat";
			break;
		case JIT_TYPE_STRUCT:
			std::cout << "struct";
			break;
		case JIT_TYPE_UNION:
			std::cout << "union";
			break;
		case JIT_TYPE_SIGNATURE:
			std::cout << "function";
			break;
		case JIT_TYPE_PTR:
			std::cout << "ptr";
			break;
		default:
			break;
	}
	if(isPointer) std::cout << '*';
}

int main()
{
	RegularFile f(WASMPATH, Elv::Io::Mode::READ);
	std::cout << WASMPATH << "\nSize: " << f.size() << std::endl;

	WASM::Module mod;
	mod.fromFile(f);
	/*for(const auto& it : mod.getSections())
	{
		std::cout << "Section type: " << static_cast<int>(it.type) << " (" << WASM::getSectionTypeName(it.type) << ")\n"
				  << "Section offset: " << it.offset << "\nSection size: " << it.size << "\n===\n";
	}
	std::cout << std::endl;*/
	std::vector<jit_type_t> types;
	LibJIT::Context context;
	context.getTranslator().translateTypes(mod.types);
	types = context.getTranslator().getTranslatedTypes();
	for(size_t i = 0; i < types.size(); ++i) {
		jit_type_t type = types[i];
		std::cout << '[' << i << "] Type: ";
		printType(type);
		std::cout << std::endl;
	}
	for(size_t i = 0; i < mod.functionBodies.size(); ++i) {
		const auto& body = mod.functionBodies[i];
		Euph::Io::ConstBufferDevice buff(Euph::Io::ConstBufferDevice::span_cast<uint8_t>(body.code));
		std::cout << "\n\n-------------\nFUNCTION # " << i << std::endl;
		WASM::WasmStream stream(buff);
		Stub::OpcodeDispatcher dispatcher(&std::cout);
		dispatcher.readCode(stream);
	}

	return 0;
}
