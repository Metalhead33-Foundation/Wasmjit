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

// Forward declarations: the typed forms come next to the JSON ones and the
// JSON ones call them.
std::string luaType(const ValueType& type);
std::string swiftType(const ValueType& type);
std::string dType(const ValueType& type);
std::string nimType(const ValueType& type);
std::string crystalType(const ValueType& type);
std::string adaType(const ValueType& type);
std::string pascalType(const ValueType& type);
std::string haskellType(const ValueType& type);

std::string pythonType(const Json& type)
{
	return pythonType(ValueType::fromJson(type));
}

std::string pythonType(const ValueType& type)
{
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::U32:
	case ValueKind::S32:
	case ValueKind::I8:
	case ValueKind::U8:
	case ValueKind::I16:
	case ValueKind::U16: return "ctypes.c_int32";
	case ValueKind::I64:
	case ValueKind::U64:
	case ValueKind::S64: return "ctypes.c_int64";
	case ValueKind::F32: return "ctypes.c_float";
	case ValueKind::F64: return "ctypes.c_double";
	case ValueKind::V128: return "ctypes.c_ubyte * 16";
	case ValueKind::Void: return "None";
	default: break;
	}
	return "ctypes.c_void_p";
}

std::string emitPythonImpl(const Json& schema, const Schema& typed)
{
	std::ostringstream out;
	out << "# Generated Python ctypes bindings from the "
		<< typed.id << " "
		<< directionName(typed.direction) << " schema. Do not edit.\n";
	out << "import ctypes\n\n";
	out << "class WasmExternref(ctypes.Structure):\n\tpass\n\n";
	std::set<std::string> witTypes;
	collectWitTypeNames(schema, witTypes);
	for (const std::string& type : witTypes)
		out << "class " << type << "(ctypes.Structure):\n\tpass\n";
	out << "\n";
	forEachFunction(typed, true, [&](const Entry& entry) {
		const std::string name = "wasmjit_" + sanitize(importModule(entry)) + "_"
				+ sanitize(entry.name);
		out << "# G2H import: " << importComment(entry) << "\n";
		out << name << "_type = ctypes.CFUNCTYPE("
			<< pythonType(entry.function.results.size() == 1
					? entry.function.results[0] : voidValue());
		for (const ValueType& param : entry.function.params)
			out << ", " << pythonType(param);
		if (entry.function.results.size() > 1u)
			out << ", ctypes.c_void_p";
		out << ")\n\n";
	});
	forEachFunction(typed, false, [&](const Entry& entry) {
		const std::string name = "wasmjit_" + sanitize(entry.name);
		out << "# H2G export: " << exportComment(entry) << "\n";
		out << name << "_type = ctypes.CFUNCTYPE("
			<< pythonType(entry.function.results.size() == 1
					? entry.function.results[0] : voidValue());
		for (const ValueType& param : entry.function.params)
			out << ", " << pythonType(param);
		if (entry.function.results.size() > 1u)
			out << ", ctypes.c_void_p";
		out << ")\n";
		out << name << " = " << name << "_type()\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string emitRubyImpl(const Json& schema, const Schema& typed)
{
	std::ostringstream out;
	out << "# Generated Ruby/Fiddle bindings from the "
		<< typed.id << " "
		<< directionName(typed.direction) << " schema. Do not edit.\n";
	out << "require 'fiddle'\nrequire 'fiddle/import'\n\n";
	out << "module WasmJitBindings\n\textend Fiddle::Importer\n\n";
	std::set<std::string> witTypes;
	collectWitTypeNames(schema, witTypes);
	for (const std::string& type : witTypes)
		out << "\t" << pascalName(type) << " = Object\n";
	out << "\n";
	out << "\t# Use dlload with the host's Wasm bridge library before calling.\n";
	forEachFunction(typed, true, [&](const Entry& entry) {
		out << "\t# G2H import: " << importComment(entry) << "\n";
		out << "\textern 'void* wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.name) << "'\n\n";
	});
	forEachFunction(typed, false, [&](const Entry& entry) {
		out << "\t# H2G export: " << exportComment(entry)
			<< "\n\t# type: Fiddle::TYPE_VOIDP\n\n";
	});
	out << nonFunctionNotes(schema);
	out << "end\n";
	return out.str();
}

std::string luaType(const Json& type)
{
	return luaType(ValueType::fromJson(type));
}

std::string luaType(const ValueType& type)
{
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::U32:
	case ValueKind::S32:
	case ValueKind::I8:
	case ValueKind::U8:
	case ValueKind::I16:
	case ValueKind::U16: return "int32_t";
	case ValueKind::I64:
	case ValueKind::U64:
	case ValueKind::S64: return "int64_t";
	case ValueKind::F32: return "float";
	case ValueKind::F64: return "double";
	case ValueKind::V128: return "uint8_t[16]";
	case ValueKind::Void: return "void";
	default: break;
	}
	return "void*";
}

std::string emitLuaImpl(const Json& schema, const Schema& typed)
{
	std::ostringstream out;
	out << "-- Generated LuaJIT FFI bindings from the "
		<< typed.id << " "
		<< directionName(typed.direction) << " schema. Do not edit.\n";
	out << "local ffi = require(\"ffi\")\n\nffi.cdef[[\n";
	std::set<std::string> witTypes;
	collectWitTypeNames(schema, witTypes);
	for (const std::string& type : witTypes)
		out << "typedef struct " << type << " " << type << ";\n";
	forEachFunction(typed, false, [&](const Entry& entry) {
		out << "typedef " << (entry.function.results.size() == 1
				? luaType(entry.function.results[0]) : "void")
			<< " (*wasmjit_" << sanitize(entry.name)
			<< "_fn)(";
		const std::vector<ValueType>& params = entry.function.params;
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << luaType(params[i]);
		}
		if (entry.function.results.size() > 1u)
			out << (params.empty() ? "" : ", ") << "void*";
		out << ");\n";
	});
	forEachFunction(typed, true, [&](const Entry& entry) {
		out << luaType(entry.function.results.size() == 1
				? entry.function.results[0] : voidValue())
			<< " wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.name) << "(";
		const std::vector<ValueType>& params = entry.function.params;
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << luaType(params[i]);
		}
		if (entry.function.results.size() > 1u)
			out << (params.empty() ? "" : ", ") << "void*";
		out << ");\n";
	});
	out << "]]\n\n";
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string swiftType(const Json& type)
{
	return swiftType(ValueType::fromJson(type));
}

std::string swiftType(const ValueType& type)
{
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::U32:
	case ValueKind::S32:
	case ValueKind::I8:
	case ValueKind::U8:
	case ValueKind::I16:
	case ValueKind::U16: return "Int32";
	case ValueKind::I64:
	case ValueKind::U64:
	case ValueKind::S64: return "Int64";
	case ValueKind::F32: return "Float";
	case ValueKind::F64: return "Double";
	case ValueKind::Void: return "Void";
	default: break;
	}
	return "UnsafeMutableRawPointer?";
}

std::string emitSwiftImpl(const Json& schema, const Schema& typed)
{
	std::ostringstream out;
	out << "// Generated Swift/C-convention bindings from the "
		<< typed.id << " "
		<< directionName(typed.direction) << " schema. Do not edit.\n";
	out << "import Foundation\n\n";
	out << "public typealias WasmExternref = UnsafeMutableRawPointer\n\n";
	std::set<std::string> witTypes;
	collectWitTypeNames(schema, witTypes);
	for (const std::string& type : witTypes)
		out << "public struct " << type << " { }\n";
	out << "\n";
	forEachFunction(typed, false, [&](const Entry& entry) {
		const std::string name = sanitize(entry.name);
		out << "public typealias Wasmjit_" << name << "_Fn = @convention(c) (";
		const std::vector<ValueType>& params = entry.function.params;
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << swiftType(params[i]);
		}
		if (entry.function.results.size() > 1u)
			out << (params.empty() ? "" : ", ") << "UnsafeMutableRawPointer?";
		out << ") -> "
			<< (entry.function.results.size() == 1
					? swiftType(entry.function.results[0]) : "Void")
			<< "\n\n";
	});
	forEachFunction(typed, true, [&](const Entry& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		out << "@_silgen_name(\"" << entry.name << "\")\n";
		out << "public func wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.name) << "(";
		const std::vector<ValueType>& params = entry.function.params;
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << argumentName(entry, i) << ": " << swiftType(params[i]);
		}
		if (entry.function.results.size() > 1u)
			out << (params.empty() ? "" : ", ") << "result: UnsafeMutableRawPointer";
		out << ") -> "
			<< (entry.function.results.size() == 1
					? swiftType(entry.function.results[0]) : "Void") << "\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}


std::string dType(const Json& type)
{
	return dType(ValueType::fromJson(type));
}

std::string dType(const ValueType& type)
{
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::U32:
	case ValueKind::S32:
	case ValueKind::I8:
	case ValueKind::U8:
	case ValueKind::I16:
	case ValueKind::U16: return "int";
	case ValueKind::I64:
	case ValueKind::U64:
	case ValueKind::S64: return "long";
	case ValueKind::F32: return "float";
	case ValueKind::F64: return "double";
	case ValueKind::V128: return "ubyte[16]";
	case ValueKind::Void: return "void";
	default: break;
	}
	return "void*";
}

std::string emitDLangImpl(const Json& schema, const Schema& typed)
{
	std::ostringstream out;
	out << "// Generated D extern(C) bindings from the "
		<< typed.id << " "
		<< directionName(typed.direction) << " schema. Do not edit.\n";
	out << "extern(C):\n\n";
	std::set<std::string> witTypes;
	collectWitTypeNames(schema, witTypes);
	for (const std::string& type : witTypes)
		out << "struct " << type << " { }\n";
	out << "\n";
	forEachFunction(typed, false, [&](const Entry& entry) {
		const std::string name = sanitize(entry.name);
		out << "alias Wasmjit_" << name << "_Fn = " << dType(
				entry.function.results.size() == 1
						? entry.function.results[0] : voidValue())
			<< " function(";
		const std::vector<ValueType>& params = entry.function.params;
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << dType(params[i]);
		}
		if (entry.function.results.size() > 1u)
			out << (params.empty() ? "" : ", ") << "void*";
		out << ");\n\n";
	});
	forEachFunction(typed, true, [&](const Entry& entry) {
		out << "// G2H import: " << importComment(entry) << "\n";
		out << dType(entry.function.results.size() == 1
				? entry.function.results[0] : voidValue())
			<< " wasmjit_" << sanitize(importModule(entry)) << "_"
			<< sanitize(entry.name) << "(";
		const std::vector<ValueType>& params = entry.function.params;
		for (std::size_t i = 0; i < params.size(); ++i) {
			if (i != 0) out << ", ";
			out << dType(params[i]);
		}
		if (entry.function.results.size() > 1u)
			out << (params.empty() ? "" : ", ") << "void*";
		out << ");\n\n";
	});
	out << nonFunctionNotes(schema);
	return out.str();
}

std::string nimType(const Json& type)
{
	return nimType(ValueType::fromJson(type));
}

std::string nimType(const ValueType& type)
{
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::U32:
	case ValueKind::S32:
	case ValueKind::I8:
	case ValueKind::U8:
	case ValueKind::I16:
	case ValueKind::U16: return "int32";
	case ValueKind::I64:
	case ValueKind::U64:
	case ValueKind::S64: return "int64";
	case ValueKind::F32: return "float32";
	case ValueKind::F64: return "float64";
	case ValueKind::V128: return "array[16, uint8]";
	case ValueKind::Void: return "void";
	default: break;
	}
	return "pointer";
}

std::string emitNimImpl(const Json& schema)
{
	std::ostringstream out;
	out << "# Generated Nim bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	std::set<std::string> witTypes;
	collectWitTypeNames(schema, witTypes);
	for (const std::string& type : witTypes)
		out << "type " << type << "* = object\n";
	out << "\n";
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
	return crystalType(ValueType::fromJson(type));
}

std::string crystalType(const ValueType& type)
{
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::U32:
	case ValueKind::S32:
	case ValueKind::I8:
	case ValueKind::U8:
	case ValueKind::I16:
	case ValueKind::U16: return "Int32";
	case ValueKind::I64:
	case ValueKind::U64:
	case ValueKind::S64: return "Int64";
	case ValueKind::F32: return "Float32";
	case ValueKind::F64: return "Float64";
	case ValueKind::V128: return "StaticArray(UInt8, 16)";
	case ValueKind::Void: return "Void";
	default: break;
	}
	return "Void*";
}

std::string emitCrystalImpl(const Json& schema)
{
	std::ostringstream out;
	out << "# Generated Crystal lib bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit.\n";
	out << "lib WasmJitBindings\n";
	std::set<std::string> witTypes;
	collectWitTypeNames(schema, witTypes);
	for (const std::string& type : witTypes)
		out << "  struct " << type << "\n  end\n";
	out << "\n";
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
	return adaType(ValueType::fromJson(type));
}

std::string adaType(const ValueType& type)
{
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::U32:
	case ValueKind::S32:
	case ValueKind::I8:
	case ValueKind::U8:
	case ValueKind::I16:
	case ValueKind::U16: return "Interfaces.Integer_32";
	case ValueKind::I64:
	case ValueKind::U64:
	case ValueKind::S64: return "Interfaces.Integer_64";
	case ValueKind::F32: return "Interfaces.Float_32";
	case ValueKind::F64: return "Interfaces.Float_64";
	case ValueKind::Void: return "Ada.Numerics.Void";
	default: break;
	}
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
	std::set<std::string> witTypes;
	collectWitTypeNames(schema, witTypes);
	for (const std::string& type : witTypes)
		out << "   type " << pascalName(type) << " is new System.Address;\n";
	out << "\n";
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
	return pascalType(ValueType::fromJson(type));
}

std::string pascalType(const ValueType& type)
{
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::U32:
	case ValueKind::S32:
	case ValueKind::I8:
	case ValueKind::U8:
	case ValueKind::I16:
	case ValueKind::U16: return "LongInt";
	case ValueKind::I64:
	case ValueKind::U64:
	case ValueKind::S64: return "Int64";
	case ValueKind::F32: return "Single";
	case ValueKind::F64: return "Double";
	case ValueKind::V128: return "array[0..15] of Byte";
	case ValueKind::Void: return "";
	default: break;
	}
	return "Pointer";
}

std::string emitPascalImpl(const Json& schema)
{
	std::ostringstream out;
	out << "{ Generated Pascal bindings from the "
		<< schema.at("schema").get<std::string>() << " "
		<< schema.at("direction").get<std::string>() << " schema. Do not edit. }\n";
	out << "unit WasmJitBindings;\n\ninterface\n\ntype\n";
	std::set<std::string> witTypes;
	collectWitTypeNames(schema, witTypes);
	for (const std::string& type : witTypes)
		out << "  " << type << " = Pointer;\n";
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
	return haskellType(ValueType::fromJson(type));
}

std::string haskellType(const ValueType& type)
{
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::U32:
	case ValueKind::S32:
	case ValueKind::I8:
	case ValueKind::U8:
	case ValueKind::I16:
	case ValueKind::U16: return "CInt";
	case ValueKind::I64:
	case ValueKind::U64:
	case ValueKind::S64: return "CLLong";
	case ValueKind::F32: return "CFloat";
	case ValueKind::F64: return "CDouble";
	case ValueKind::Void: return "()";
	default: break;
	}
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
	std::set<std::string> witTypes;
	collectWitTypeNames(schema, witTypes);
	for (const std::string& type : witTypes)
		out << "data " << pascalName(type) << " = " << pascalName(type) << "\n";
	out << "\n";
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
