#include "EmitInternal.hpp"
#include "EmitShared.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace WASM::Bindings::detail {

using Json = nlohmann::ordered_json;

std::string pythonType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "ctypes.c_int32";
	if (kind == "i64" || kind == "u64" || kind == "s64")
		return "ctypes.c_int64";
	if (kind == "f32" || kind == "float32") return "ctypes.c_float";
	if (kind == "f64" || kind == "float64") return "ctypes.c_double";
	if (kind == "v128") return "ctypes.c_ubyte * 16";
	if (kind == "void") return "None";
	return "ctypes.c_void_p";
}

std::string emitPythonImpl(const Json& schema)
{
	std::ostringstream out;
	out << "# Generated Python ctypes bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "import ctypes\n\n";
	out << "class WasmExternref(ctypes.Structure):\n\tpass\n\n";
	forEachFunction(schema, "imports", [&](const Json& entry) {
		const std::string name = "wasmjit_" + sanitize(importModule(entry)) + "_"
				+ sanitize(entry.at("name").get<std::string>());
		out << "# G2H import: " << importComment(entry) << "\n";
		out << name << "_type = ctypes.CFUNCTYPE("
			<< pythonType(signatureResults(entry).size() == 1
					? signatureResults(entry)[0] : Json{{"type", "void"}});
		for (const Json& param : signatureParams(entry))
			out << ", " << pythonType(param);
		if (signatureResults(entry).size() > 1u)
			out << ", ctypes.c_void_p";
		out << ")\n\n";
	});
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = "wasmjit_" + sanitize(entry.at("name").get<std::string>());
		out << "# H2G export: " << exportComment(entry) << "\n";
		out << name << "_type = ctypes.CFUNCTYPE("
			<< pythonType(signatureResults(entry).size() == 1
					? signatureResults(entry)[0] : Json{{"type", "void"}});
		for (const Json& param : signatureParams(entry))
			out << ", " << pythonType(param);
		if (signatureResults(entry).size() > 1u)
			out << ", ctypes.c_void_p";
		out << ")\n";
		out << name << " = " << name << "_type()\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string emitRubyImpl(const Json& schema)
{
	std::ostringstream out;
	out << "# Generated Ruby/Fiddle bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "require 'fiddle'\nrequire 'fiddle/import'\n\n";
	out << "module WasmJitBindings\n\textend Fiddle::Importer\n\n";
	out << "\t# Use dlload with the host's Wasm bridge library before calling.\n";
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "\t# G2H import: " << importComment(entry) << "\n";
		out << "\textern 'void* wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "'\n\n";
	});
	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "\t# H2G export: " << exportComment(entry)
			<< "\n\t# type: Fiddle::TYPE_VOIDP\n\n";
	});
	out << nonFunctionNotes(schema);
	out << "end\n";
	return out.str();
}

std::string luaType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "int32_t";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "int64_t";
	if (kind == "f32" || kind == "float32") return "float";
	if (kind == "f64" || kind == "float64") return "double";
	if (kind == "v128") return "uint8_t[16]";
	if (kind == "void") return "void";
	return "void*";
}

std::string emitLuaImpl(const Json& schema)
{
	std::ostringstream out;
	out << "-- Generated LuaJIT FFI bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "local ffi = require(\"ffi\")\n\nffi.cdef[[\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "typedef " << (signatureResults(entry).size() == 1
				? luaType(signatureResults(entry)[0]) : "void")
			<< " (*wasmjit_" << sanitize(entry.at("name").get<std::string>())
			<< "_fn)(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << luaType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "void*";
		out << ");\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << luaType(signatureResults(entry).size() == 1
				? signatureResults(entry)[0] : Json{{"type", "void"}})
			<< " wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << luaType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "void*";
		out << ");\n";
	});
	out << "]]\n\n";
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string swiftType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "Int32";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "Int64";
	if (kind == "f32" || kind == "float32") return "Float";
	if (kind == "f64" || kind == "float64") return "Double";
	if (kind == "void") return "Void";
	return "UnsafeMutableRawPointer?";
}

std::string emitSwiftImpl(const Json& schema)
{
	std::ostringstream out;
	out << "// Generated Swift/C-convention bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "import Foundation\n\n";
	out << "public typealias WasmExternref = UnsafeMutableRawPointer\n\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = sanitize(entry.at("name").get<std::string>());
		out << "public typealias Wasmjit_" << name << "_Fn = @convention(c) (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << swiftType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "UnsafeMutableRawPointer?";
		out << ") -> "
			<< (signatureResults(entry).size() == 1
					? swiftType(signatureResults(entry)[0]) : "Void")
			<< "\n\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		out << "@_silgen_name(\"" << entry.at("name").get<std::string>() << "\")\n";
		out << "public func wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << ": " << swiftType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "result: UnsafeMutableRawPointer";
		out << ") -> "
			<< (signatureResults(entry).size() == 1
					? swiftType(signatureResults(entry)[0]) : "Void") << "\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string dType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "int";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "long";
	if (kind == "f32" || kind == "float32") return "float";
	if (kind == "f64" || kind == "float64") return "double";
	if (kind == "v128") return "ubyte[16]";
	if (kind == "void") return "void";
	return "void*";
}

std::string emitDLangImpl(const Json& schema)
{
	std::ostringstream out;
	out << "// Generated D extern(C) bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "extern(C):\n\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = sanitize(entry.at("name").get<std::string>());
		out << "alias Wasmjit_" << name << "_Fn = " << dType(
				signatureResults(entry).size() == 1
						? signatureResults(entry)[0] : Json{{"type", "void"}})
			<< " function(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << dType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "void*";
		out << ");\n\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		out << dType(signatureResults(entry).size() == 1
				? signatureResults(entry)[0] : Json{{"type", "void"}})
			<< " wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << dType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "void*";
		out << ");\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string nimType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "int32";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "int64";
	if (kind == "f32" || kind == "float32") return "float32";
	if (kind == "f64" || kind == "float64") return "float64";
	if (kind == "v128") return "array[16, uint8]";
	if (kind == "void") return "void";
	return "pointer";
}

std::string emitNimImpl(const Json& schema)
{
	std::ostringstream out;
	out << "# Generated Nim bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = sanitize(entry.at("name").get<std::string>());
		out << "type Wasmjit_" << name << "_Fn = proc(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << "; ";
			out << argumentName(entry, i) << ": " << nimType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : "; ") << "result: pointer";
		out << "): " << (signatureResults(entry).size() == 1
				? nimType(signatureResults(entry)[0]) : "void")
			<< " {.cdecl.}\n\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "proc wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << "; ";
			out << argumentName(entry, i) << ": " << nimType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : "; ") << "result: pointer";
		out << "): " << (signatureResults(entry).size() == 1
				? nimType(signatureResults(entry)[0]) : "void")
			<< " {.importc: \"" << entry.at("name").get<std::string>()
			<< "\", cdecl.}\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string crystalType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "Int32";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "Int64";
	if (kind == "f32" || kind == "float32") return "Float32";
	if (kind == "f64" || kind == "float64") return "Float64";
	if (kind == "v128") return "StaticArray(UInt8, 16)";
	if (kind == "void") return "Void";
	return "Void*";
}

std::string emitCrystalImpl(const Json& schema)
{
	std::ostringstream out;
	out << "# Generated Crystal lib bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "lib WasmJitBindings\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = sanitize(entry.at("name").get<std::string>());
		out << "  alias Wasmjit_" << name << "_Fn = (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << crystalType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "Void*";
		out << ") -> " << (signatureResults(entry).size() == 1
				? crystalType(signatureResults(entry)[0]) : "Void") << "\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "  # G2H import: " << importComment(entry) << "\n";
		out << "  fun wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << " : " << crystalType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : ", ") << "result : Void*";
		out << ") : " << (signatureResults(entry).size() == 1
				? crystalType(signatureResults(entry)[0]) : "Void") << "\n";
	});
	out << "end\n";
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string adaType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "Interfaces.Integer_32";
	if (kind == "i64" || kind == "u64" || kind == "s64")
		return "Interfaces.Integer_64";
	if (kind == "f32" || kind == "float32") return "Interfaces.Float_32";
	if (kind == "f64" || kind == "float64") return "Interfaces.Float_64";
	if (kind == "void") return "Ada.Numerics.Void";
	return "System.Address";
}

std::string emitAdaImpl(const Json& schema)
{
	std::ostringstream out;
	out << "-- Generated Ada bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "with Interfaces; use Interfaces;\n";
	out << "with System;\n\npackage WasmJit.Bindings is\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = sanitize(entry.at("name").get<std::string>());
		out << "   type Wasmjit_" << name << "_Fn is access function (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << "; ";
			out << pascalName(argumentName(entry, i)) << " : " << adaType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : "; ") << "Result : System.Address";
		out << ") return " << (signatureResults(entry).size() == 1
				? adaType(signatureResults(entry)[0]) : "System.Address")
			<< " with Convention => C;\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "   -- G2H import: " << importComment(entry) << "\n";
		out << "   function Wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << " (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << "; ";
			out << pascalName(argumentName(entry, i)) << " : " << adaType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : "; ") << "Result : System.Address";
		out << ") return " << (signatureResults(entry).size() == 1
				? adaType(signatureResults(entry)[0]) : "System.Address")
			<< " with Import, Convention => C, External_Name => \""
			<< entry.at("name").get<std::string>() << "\";\n";
	});
	out << "end WasmJit.Bindings;\n";
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string pascalType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "LongInt";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "Int64";
	if (kind == "f32" || kind == "float32") return "Single";
	if (kind == "f64" || kind == "float64") return "Double";
	if (kind == "v128") return "array[0..15] of Byte";
	if (kind == "void") return "";
	return "Pointer";
}

std::string emitPascalImpl(const Json& schema)
{
	std::ostringstream out;
	out << "{ Generated Pascal bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit. }\n";
	out << "unit WasmJitBindings;\n\ninterface\n\ntype\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		const std::string name = sanitize(entry.at("name").get<std::string>());
		out << "  Wasmjit_" << name << "_Fn = function (";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << "; ";
			out << argumentName(entry, i) << ": " << pascalType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : "; ") << "result: Pointer";
		out << "): " << (signatureResults(entry).size() == 1
				? pascalType(signatureResults(entry)[0]) : "LongInt")
			<< "; cdecl;\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "  // G2H import: " << importComment(entry) << "\n";
		out << "  function wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "(";
		const Json& params = signatureParams(entry);
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << "; ";
			out << argumentName(entry, i) << ": " << pascalType(params[i]);
		}
		if (signatureResults(entry).size() > 1u)
			out << (params.empty() ? "" : "; ") << "result: Pointer";
		out << "): " << (signatureResults(entry).size() == 1
				? pascalType(signatureResults(entry)[0]) : "LongInt")
			<< "; cdecl; external '" << importModule(entry) << "' name '"
			<< entry.at("name").get<std::string>() << "';\n";
	});
	out << "\nimplementation\n\nend.\n";
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string haskellType(const Json& type)
{
	const std::string kind = type.value("type", "");
	if (kind == "i32" || kind == "u32" || kind == "s32" || kind == "i8"
			|| kind == "u8" || kind == "i16" || kind == "u16")
		return "CInt";
	if (kind == "i64" || kind == "u64" || kind == "s64") return "CLLong";
	if (kind == "f32" || kind == "float32") return "CFloat";
	if (kind == "f64" || kind == "float64") return "CDouble";
	if (kind == "void") return "()";
	return "Ptr ()";
}

std::string emitHaskellImpl(const Json& schema)
{
	std::ostringstream out;
	out << "-- Generated Haskell FFI bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "{-# LANGUAGE ForeignFunctionInterface #-}\n";
	out << "module WasmJitBindings where\n\n";
	out << "import Foreign.C.Types\nimport Foreign.Ptr\n\n";
	forEachFunction(schema, "exports", [&](const Json& entry) {
		out << "-- H2G export: " << exportComment(entry) << "\n";
		out << "type Wasmjit_" << sanitize(entry.at("name").get<std::string>())
			<< "_Fn = ";
		const Json& params = signatureParams(entry);
		for (const Json& param : params)
			out << haskellType(param) << " -> ";
		if (signatureResults(entry).size() > 1u)
			out << "Ptr () -> ";
		out << "IO " << (signatureResults(entry).size() == 1
				? haskellType(signatureResults(entry)[0]) : "()") << "\n\n";
	});
	forEachFunction(schema, "imports", [&](const Json& entry) {
		out << "foreign import ccall \"" << entry.at("name").get<std::string>()
			<< "\" wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.at("name").get<std::string>()) << "\n  :: ";
		for (const Json& param : signatureParams(entry))
			out << haskellType(param) << " -> ";
		if (signatureResults(entry).size() > 1u)
			out << "Ptr () -> ";
		out << "IO " << (signatureResults(entry).size() == 1
				? haskellType(signatureResults(entry)[0]) : "()") << "\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}

} // namespace WASM::Bindings::detail
