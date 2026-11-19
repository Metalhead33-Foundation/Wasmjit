#include "EmitObjects.hpp"

#include "EmitShared.hpp"

#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace WASM::Bindings::detail {

namespace {

//! Lower-cases a copy of `text`.
std::string lowerCopy(std::string text)
{
	for (char& character : text)
		character = static_cast<char>(
				std::tolower(static_cast<unsigned char>(character)));
	return text;
}

//! Upper-cases a copy of `text`.
std::string upperCopy(std::string text)
{
	for (char& character : text)
		character = static_cast<char>(
				std::toupper(static_cast<unsigned char>(character)));
	return text;
}

//! The `objects` array, or null when the schema carries none.
const std::vector<ObjectDescriptor>* objectList(const Schema& schema)
{
	return schema.objects.empty() ? nullptr : &schema.objects;
}

//! The current version of a named descriptor, or null.
//!
//! A name is a lookup key, not an identity (A.14.2): when several versions are
//! declared, the highest one is what the name means.
const ObjectDescriptor* namedObject(const Schema& schema, std::string_view name)
{
	const ObjectDescriptor* found = nullptr;
	for (const ObjectDescriptor& object : schema.objects) {
		if (object.name != name)
			continue;
		if (found == nullptr || object.version > found->version)
			found = &object;
	}
	return found;
}

//! The macro prefix of an object: `WASMJIT_OBJECT_DEMO_WIDGET_V1`.
std::string macroPrefix(const ObjectDescriptor& object)
{
	return upperCopy(objectTypeName(object));
}

//! A WIT identifier built from an arbitrary name (`demo.Widget` -> `demo-widget`).
std::string witIdentifier(std::string text)
{
	std::string out;
	for (const char character : text) {
		if (std::isalnum(static_cast<unsigned char>(character)))
			out.push_back(static_cast<char>(
					std::tolower(static_cast<unsigned char>(character))));
		else if (out.empty() || out.back() != '-')
			out.push_back('-');
	}
	while (!out.empty() && out.back() == '-')
		out.pop_back();
	if (out.empty())
		out = "object";
	if (!std::isalpha(static_cast<unsigned char>(out.front())))
		out.insert(0, "object-");
	return out;
}

//! The WIT resource name of one object: `demo-widget-v1`.
std::string witObjectName(const ObjectDescriptor& object)
{
	return witIdentifier(object.name) + "-v" + std::to_string(object.version);
}

//! The C spelling of a descriptor type, resolving a defined reference to the
//! object it names.
std::string objectCType(const Schema& schema, const ValueType& type)
{
	if (type.kind == ValueKind::Ref) {
		if (type.heap == HeapKind::Defined) {
			if (const ObjectDescriptor* target = namedObject(schema, type.name))
				return objectTypeName(*target) + "*";
		}
		return refTypeName(type) + "*";
	}
	return cType(type);
}

//! The Kotlin spelling of one descriptor type, or nullopt when it is not a scalar.
//!
//! The Kotlin façade wraps the slot, not the layout, so only the scalar
//! parameter types a constructor can take are spelled here; a reference or a
//! `v128` is reported as not expressible, and the constructor is omitted with a
//! comment rather than emitted with a guessed type.
std::optional<std::string> objectKotlinType(const ValueType& type)
{
	if (type.kind == ValueKind::Ref)
		return std::nullopt;
	if (type.packed)
		return type.kind == ValueKind::I8 ? "Byte" : "Short";
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::S32: return "Int";
	case ValueKind::U32: return "UInt";
	case ValueKind::I64:
	case ValueKind::S64: return "Long";
	case ValueKind::U64: return "ULong";
	case ValueKind::F32: return "Float";
	case ValueKind::F64: return "Double";
	default: break;
	}
	return std::nullopt;
}

//! The Rust spelling of a descriptor type, with the same resolution as C.
std::string objectRustType(const Schema& schema, const ValueType& type)
{
	if (type.kind == ValueKind::Ref && type.heap == HeapKind::Defined) {
		if (const ObjectDescriptor* target = namedObject(schema, type.name))
			return "*mut " + objectTypeName(*target);
	}
	return rustType(type);
}

//! The descriptor spelling of a type, for the metadata tables (`i32`,
//! `ref extern`, `ref demo.Node`).
std::string descriptorTypeName(const ValueType& type)
{
	std::string out = valueKindName(type.kind);
	if (type.kind == ValueKind::Ref) {
		out += " ";
		out += heapKindName(type.heap);
		if (!type.name.empty())
			out += " " + type.name;
	}
	if (type.packed)
		out += " (packed)";
	return out;
}

//! The WIT spelling of one descriptor type, or nullopt when WIT cannot say it.
std::optional<std::string> witType(const Schema& schema, const ValueType& type)
{
	if (type.kind == ValueKind::Ref) {
		// A reference to another host object is a borrowed handle; every other
		// reference (externref, anyref, ...) has no WIT spelling.
		if (type.heap != HeapKind::Defined)
			return std::nullopt;
		const ObjectDescriptor* target = namedObject(schema, type.name);
		if (target == nullptr)
			return std::nullopt;
		return "borrow<" + witObjectName(*target) + ">";
	}
	if (type.packed) {
		if (type.kind == ValueKind::I8) return "s8";
		if (type.kind == ValueKind::I16) return "s16";
		return std::nullopt;
	}
	switch (type.kind) {
	case ValueKind::I32:
	case ValueKind::S32: return "s32";
	case ValueKind::I64:
	case ValueKind::S64: return "s64";
	case ValueKind::F32: return "f32";
	case ValueKind::F64: return "f64";
	default: break;
	}
	return std::nullopt;
}

} // namespace

std::string objectIdentifier(std::string_view name)
{
	return sanitize(lowerCopy(std::string(name)));
}

std::string objectTypeName(const ObjectDescriptor& object)
{
	return "wasmjit_object_" + objectIdentifier(object.name) + "_v"
			+ std::to_string(object.version);
}

void emitCObjects(std::ostringstream& out, const Schema& schema)
{
	const std::vector<ObjectDescriptor>* objects = objectList(schema);
	if (objects == nullptr)
		return;

	out << "/* Host-registered objects (wasmjit.binding/2).\n"
		   "   A descriptor is host metadata, not a Wasm ABI: the offsets are the\n"
		   "   canonical, header-inclusive offsets WASM::layoutHostType derives, and\n"
		   "   a reference field is a GC reference, never an integer handle. Field\n"
		   "   access goes through the engine's object API, not these constants. */\n\n";
	out << "typedef struct wasmjit_object_meta_field {\n"
		   "    const char* name;\n"
		   "    uint32_t offset;\n"
		   "    uint32_t bytes;\n"
		   "    uint8_t is_reference;\n"
		   "    uint8_t is_mutable;\n"
		   "} wasmjit_object_meta_field;\n\n";
	out << "typedef struct wasmjit_object_property_meta {\n"
		   "    const char* name;\n"
		   "    const char* type;\n"
		   "    const char* get_export;\n"
		   "    const char* set_export;\n"
		   "    uint8_t is_mutable;\n"
		   "} wasmjit_object_property_meta;\n\n";
	out << "typedef struct wasmjit_object_meta {\n"
		   "    const char* name;\n"
		   "    uint32_t version;\n"
		   "    uint32_t size;\n"
		   "    const char* parent;\n"
		   "    uint32_t field_count;\n"
		   "    const wasmjit_object_meta_field* fields;\n"
		   "    uint32_t property_count;\n"
		   "    const wasmjit_object_property_meta* properties;\n"
		   "    uint32_t method_count;\n"
		   "    const char* const* methods;\n"
		   "} wasmjit_object_meta;\n\n";

	out << "/* One opaque handle per (name, version) descriptor. A C host cannot spell\n"
		   "   `(ref $T)`, so this is the type it passes; every typedef is emitted\n"
		   "   first, so a method signature may name any object regardless of\n"
		   "   declaration order. */\n";
	for (const ObjectDescriptor& object : *objects) {
		const std::string type = objectTypeName(object);
		out << "typedef struct " << type << " " << type << ";\n";
	}
	out << "\n";

	for (const ObjectDescriptor& object : *objects) {
		const std::string type = objectTypeName(object);
		const std::string macro = macroPrefix(object);
		const std::string& name = object.name;
		const std::uint32_t version = object.version;
		const std::uint32_t size = object.size;

		out << "/* " << name << " version " << version;
		if (!object.parent.empty())
			out << ", parent " << object.parent;
		if (!object.interfaces.empty()) {
			out << ", interfaces ";
			bool first = true;
			for (const std::string& entry : object.interfaces) {
				if (!first)
					out << ", ";
				first = false;
				out << entry;
			}
		}
		out << ". */\n";
		out << "#define " << macro << "_VERSION " << version << "u\n";
		out << "#define " << macro << "_SIZE " << size << "u\n";
		for (const ObjectField& field : object.fields) {
			out << "#define " << macro << "_FIELD_"
				<< upperCopy(objectIdentifier(field.name)) << " " << field.offset
				<< "u\n";
		}
		for (const ObjectMethod& method : object.methods) {
			out << "typedef ";
			if (method.results.size() == 1u)
				out << cType(method.results.at(0));
			else
				out << "void";
			out << " (*" << type << "_" << objectIdentifier(method.name)
				<< "_fn)(" << type << "* self";
			for (std::size_t index = 0; index < method.params.size(); ++index) {
				out << ", " << objectCType(schema, method.params.at(index)) << " "
					<< objectIdentifier(method.paramName(index));
			}
			if (method.results.size() > 1u)
				out << ", void* results";
			out << ");\n";
		}
		if (!object.properties.empty()) {
			out << "\n/* Properties: a value reached through a function, never a slot.\n"
				   "   The accessor names are the instance's export names; a host\n"
				   "   binds one trampoline per name and reads the property through\n"
				   "   it, so a getter may compute or cascade rather than load. */\n";
			for (const ObjectProperty& property : object.properties) {
				const std::string propName = property.name;
				const std::string propIdent = objectIdentifier(propName);
				const std::string propMacro = macro + "_PROP_" + upperCopy(propIdent);
				out << "#define " << propMacro << "_GET "
					<< quoteName(property.getter, false) << "\n";
				if (property.mutableProperty)
					out << "#define " << propMacro << "_SET "
						<< quoteName(property.setter, false) << "\n";
				out << "typedef " << objectCType(schema, property.type)
					<< " (*" << type << "_prop_" << propIdent << "_get_fn)(" << type
					<< "* self);\n";
				if (property.mutableProperty)
					out << "typedef void (*" << type << "_prop_" << propIdent
						<< "_set_fn)(" << type << "* self, "
						<< objectCType(schema, property.type) << " value);\n";
			}
		}
		// The handle surface (A.14.4 option B): the same accessors over the `i64`
		// a non-GC consumer carries. It is a *thunk* surface — resolve, then
		// forward — so an operation keeps one implementation and two signatures.
		out << "\n/* The handle surface (A.14.4): the `i64` slot a non-GC consumer\n"
			   "   carries. A host that serves one emits a thunk per signature below\n"
			   "   over the typed accessor above, so the operation keeps one\n"
			   "   implementation. The slot is host state: this header declares the\n"
			   "   spelling, never the table. */\n";
		out << "typedef int64_t " << type << "_handle;\n";
		out << "#define " << macro << "_HANDLE_NONE 0\n";
		// A constructor is the one entry point that has no `self`: it takes the
		// arguments and returns the slot, which is what a C++ constructor projects.
		for (const ObjectConstructor& constructor : object.constructors) {
			out << "typedef int64_t (*" << type << "_"
				<< objectIdentifier(constructor.name) << "_h_fn)(";
			if (constructor.params.empty())
				out << "void";
			for (std::size_t index = 0; index < constructor.params.size(); ++index) {
				if (index != 0)
					out << ", ";
				out << objectCType(schema, constructor.params.at(index)) << " "
					<< objectIdentifier(constructor.paramName(index));
			}
			out << ");\n";
		}
		for (const ObjectMethod& method : object.methods) {
			out << "typedef ";
			if (method.results.size() == 1u)
				out << cType(method.results.at(0));
			else
				out << "void";
			out << " (*" << type << "_" << objectIdentifier(method.name)
				<< "_h_fn)(int64_t self";
			for (std::size_t index = 0; index < method.params.size(); ++index) {
				out << ", " << objectCType(schema, method.params.at(index)) << " "
					<< objectIdentifier(method.paramName(index));
			}
			if (method.results.size() > 1u)
				out << ", void* results";
			out << ");\n";
		}
		for (const ObjectProperty& property : object.properties) {
			const std::string propIdent = objectIdentifier(property.name);
			out << "typedef " << objectCType(schema, property.type) << " (*"
				<< type << "_prop_" << propIdent << "_get_h_fn)(int64_t self);\n";
			if (property.mutableProperty)
				out << "typedef void (*" << type << "_prop_" << propIdent
					<< "_set_h_fn)(int64_t self, "
					<< objectCType(schema, property.type) << " value);\n";
		}
		// The entry points those typedefs describe. The tool declares them; the
		// embedder implements one line each — resolve the slot, then forward to
		// the typed accessor — so the operation keeps a single implementation.
		out << "\n/* The handle entry points: one per signature above, plus the release.\n"
			   "   Implement each as `resolve the slot, then forward`; the generated\n"
			   "   C++ wrapper below calls them and owns the slot for you. */\n";
		out << "void " << type << "_release(" << type << "_handle self);\n";
		for (const ObjectConstructor& constructor : object.constructors) {
			out << type << "_handle " << type << "_"
				<< objectIdentifier(constructor.name) << "(";
			if (constructor.params.empty())
				out << "void";
			for (std::size_t index = 0; index < constructor.params.size(); ++index) {
				if (index != 0)
					out << ", ";
				out << objectCType(schema, constructor.params.at(index)) << " "
					<< objectIdentifier(constructor.paramName(index));
			}
			out << ");\n";
		}
		for (const ObjectMethod& method : object.methods) {
			out << (method.results.size() == 1u ? cType(method.results.at(0))
											   : std::string("void"))
				<< " " << type << "_" << objectIdentifier(method.name) << "("
				<< type << "_handle self";
			for (std::size_t index = 0; index < method.params.size(); ++index) {
				out << ", " << objectCType(schema, method.params.at(index)) << " "
					<< objectIdentifier(method.paramName(index));
			}
			if (method.results.size() > 1u)
				out << ", void* results";
			out << ");\n";
		}
		for (const ObjectProperty& property : object.properties) {
			const std::string propIdent = objectIdentifier(property.name);
			out << objectCType(schema, property.type) << " " << type << "_prop_"
				<< propIdent << "_get(" << type << "_handle self);\n";
			if (property.mutableProperty)
				out << "void " << type << "_prop_" << propIdent << "_set(" << type
					<< "_handle self, " << objectCType(schema, property.type)
					<< " value);\n";
		}
		out << "\n";

		if (!object.fields.empty()) {
			out << "static const wasmjit_object_meta_field " << type
				<< "_fields[] = {\n";
			for (const ObjectField& field : object.fields) {
				out << "    { " << quoteName(field.name, false) << ", "
					<< field.offset << "u, " << field.bytes << "u, "
					<< (field.isReference ? 1 : 0) << "u, "
					<< (field.mutableField ? 1 : 0) << "u },\n";
			}
			out << "};\n";
		}
		if (!object.methods.empty()) {
			out << "static const char* const " << type << "_methods[] = {";
			for (std::size_t index = 0; index < object.methods.size(); ++index) {
				if (index != 0u)
					out << ", ";
				out << quoteName(object.methods.at(index).name, false);
			}
			out << "};\n";
		}
		if (!object.properties.empty()) {
			out << "static const wasmjit_object_property_meta " << type
				<< "_properties[] = {\n";
			for (const ObjectProperty& property : object.properties) {
				out << "    { " << quoteName(property.name, false) << ", "
					<< quoteName(descriptorTypeName(property.type), false) << ", "
					<< quoteName(property.getter, false) << ", ";
				if (property.mutableProperty)
					out << quoteName(property.setter, false);
				else
					out << "NULL";
				out << ", " << (property.mutableProperty ? 1 : 0) << "u },\n";
			}
			out << "};\n";
		}
		out << "static const wasmjit_object_meta " << type << "_meta = {\n"
			<< "    " << quoteName(name, false) << ", " << version << "u, " << size
			<< "u, ";
		if (!object.parent.empty())
			out << quoteName(object.parent, false);
		else
			out << "NULL";
		out << ", " << object.fields.size() << "u, "
			<< (object.fields.empty() ? std::string("NULL") : type + "_fields")
			<< ", " << object.properties.size() << "u, "
			<< (object.properties.empty() ? std::string("NULL")
										  : type + "_properties")
			<< ", " << object.methods.size() << "u, "
			<< (object.methods.empty() ? std::string("NULL") : type + "_methods")
			<< "\n};\n\n";
	}
}

void emitRustObjects(std::ostringstream& out, const Schema& schema)
{
	const std::vector<ObjectDescriptor>* objects = objectList(schema);
	if (objects == nullptr)
		return;

	out << "// Host-registered objects (wasmjit.binding/2).\n"
		   "// A descriptor is host metadata, not a Wasm ABI: the offsets are the\n"
		   "// canonical, header-inclusive offsets `layoutHostType` derives, and a\n"
		   "// reference field is a GC reference, never an integer handle.\n\n";
	out << "pub struct wasmjit_object_meta_field {\n"
		   "    pub name: &'static str,\n"
		   "    pub offset: u32,\n"
		   "    pub bytes: u32,\n"
		   "    pub is_reference: bool,\n"
		   "    pub is_mutable: bool,\n"
		   "}\n\n";
	out << "pub struct wasmjit_object_property_meta {\n"
		   "    pub name: &'static str,\n"
		   "    pub type_name: &'static str,\n"
		   "    pub get_export: &'static str,\n"
		   "    pub set_export: Option<&'static str>,\n"
		   "    pub is_mutable: bool,\n"
		   "}\n\n";
	out << "pub struct wasmjit_object_meta {\n"
		   "    pub name: &'static str,\n"
		   "    pub version: u32,\n"
		   "    pub size: u32,\n"
		   "    pub parent: Option<&'static str>,\n"
		   "    pub fields: &'static [wasmjit_object_meta_field],\n"
		   "    pub properties: &'static [wasmjit_object_property_meta],\n"
		   "    pub methods: &'static [&'static str],\n"
		   "}\n\n";

	for (const ObjectDescriptor& object : *objects) {
		const std::string type = objectTypeName(object);
		const std::string macro = macroPrefix(object);
		const std::string& name = object.name;
		const std::uint32_t version = object.version;
		const std::uint32_t size = object.size;

		out << "// " << name << " version " << version;
		if (!object.parent.empty())
			out << ", parent " << object.parent;
		if (!object.interfaces.empty()) {
			out << ", interfaces ";
			bool first = true;
			for (const std::string& entry : object.interfaces) {
				if (!first)
					out << ", ";
				first = false;
				out << entry;
			}
		}
		out << ".\n";
		out << "#[repr(C)]\npub struct " << type << " { _private: [u8; 0] }\n";
		out << "pub const " << macro << "_VERSION: u32 = " << version << ";\n";
		out << "pub const " << macro << "_SIZE: u32 = " << size << ";\n";
		for (const ObjectField& field : object.fields) {
			out << "pub const " << macro << "_FIELD_"
				<< upperCopy(objectIdentifier(field.name)) << ": u32 = "
				<< field.offset << ";\n";
		}
		for (const ObjectMethod& method : object.methods) {
			out << "pub type " << type << "_" << objectIdentifier(method.name)
				<< "_fn = unsafe extern \"C\" fn(*mut " << type;
			for (std::size_t index = 0; index < method.params.size(); ++index) {
				out << ", " << objectIdentifier(method.paramName(index)) << ": "
					<< objectRustType(schema, method.params.at(index));
			}
			if (method.results.size() > 1u)
				out << ", result: *mut core::ffi::c_void";
			out << ")";
			if (method.results.size() == 1u)
				out << " -> " << objectRustType(schema, method.results.at(0));
			out << ";\n";
		}
		if (!object.properties.empty()) {
			out << "\n";
			for (const ObjectProperty& property : object.properties) {
				const std::string propIdent = objectIdentifier(property.name);
				const std::string propMacro = macro + "_PROP_" + upperCopy(propIdent);
				out << "pub const " << propMacro << "_GET: &str = "
					<< quoteName(property.getter, true) << ";\n";
				if (property.mutableProperty)
					out << "pub const " << propMacro << "_SET: &str = "
						<< quoteName(property.setter, true) << ";\n";
				out << "pub type " << type << "_prop_" << propIdent
					<< "_get_fn = unsafe extern \"C\" fn(*mut " << type << ") -> "
					<< objectRustType(schema, property.type) << ";\n";
				if (property.mutableProperty)
					out << "pub type " << type << "_prop_" << propIdent
						<< "_set_fn = unsafe extern \"C\" fn(*mut " << type << ", "
						<< objectRustType(schema, property.type) << ");\n";
			}
		}
		// The handle surface: the same signatures over the `i64` a non-GC
		// consumer carries, so one operation keeps one implementation.
		out << "\n// The handle surface (A.14.4): the `i64` slot a non-GC consumer\n"
			   "// carries. Resolve the slot, then forward to the typed accessor.\n";
		out << "pub type " << type << "_handle = i64;\n";
		out << "pub const " << macro << "_HANDLE_NONE: i64 = 0;\n";
		for (const ObjectMethod& method : object.methods) {
			out << "pub type " << type << "_" << objectIdentifier(method.name)
				<< "_h_fn = unsafe extern \"C\" fn(i64";
			for (std::size_t index = 0; index < method.params.size(); ++index) {
				out << ", " << objectIdentifier(method.paramName(index)) << ": "
					<< objectRustType(schema, method.params.at(index));
			}
			if (method.results.size() > 1u)
				out << ", result: *mut core::ffi::c_void";
			out << ")";
			if (method.results.size() == 1u)
				out << " -> " << objectRustType(schema, method.results.at(0));
			out << ";\n";
		}
		// A constructor has no `self` and returns the slot: one alias per declared
		// constructor, mirroring the C entry points.
		for (const ObjectConstructor& constructor : object.constructors) {
			out << "pub type " << type << "_"
				<< objectIdentifier(constructor.name)
				<< "_h_fn = unsafe extern \"C\" fn(";
			for (std::size_t index = 0; index < constructor.params.size(); ++index) {
				if (index != 0)
					out << ", ";
				out << objectIdentifier(constructor.paramName(index)) << ": "
					<< objectRustType(schema, constructor.params.at(index));
			}
			out << ") -> i64;\n";
		}
		for (const ObjectProperty& property : object.properties) {
			const std::string propIdent = objectIdentifier(property.name);
			out << "pub type " << type << "_prop_" << propIdent
				<< "_get_h_fn = unsafe extern \"C\" fn(i64) -> "
				<< objectRustType(schema, property.type) << ";\n";
			if (property.mutableProperty)
				out << "pub type " << type << "_prop_" << propIdent
					<< "_set_h_fn = unsafe extern \"C\" fn(i64, "
					<< objectRustType(schema, property.type) << ");\n";
		}
		out << "\npub static " << macro
			<< "_FIELDS: &[wasmjit_object_meta_field] = &[\n";
		for (const ObjectField& field : object.fields) {
			out << "    wasmjit_object_meta_field { name: "
				<< quoteName(field.name, true) << ", offset: " << field.offset
				<< ", bytes: " << field.bytes << ", is_reference: "
				<< (field.isReference ? "true" : "false") << ", is_mutable: "
				<< (field.mutableField ? "true" : "false") << " },\n";
		}
		out << "];\n";
		if (!object.properties.empty()) {
			out << "pub static " << macro
				<< "_PROPERTIES: &[wasmjit_object_property_meta] = &[\n";
			for (const ObjectProperty& property : object.properties) {
				out << "    wasmjit_object_property_meta { name: "
					<< quoteName(property.name, true) << ", type_name: "
					<< quoteName(descriptorTypeName(property.type), true)
					<< ", get_export: " << quoteName(property.getter, true)
					<< ", set_export: ";
				if (property.mutableProperty)
					out << "Some(" << quoteName(property.setter, true) << ")";
				else
					out << "None";
				out << ", is_mutable: "
					<< (property.mutableProperty ? "true" : "false") << " },\n";
			}
			out << "];\n";
		}
		out << "pub static " << macro
			<< ": wasmjit_object_meta = wasmjit_object_meta {\n"
			<< "    name: " << quoteName(name, true) << ", version: " << version
			<< ", size: " << size << ", parent: ";
		if (!object.parent.empty())
			out << "Some(" << quoteName(object.parent, true) << ")";
		else
			out << "None";
		out << ",\n    fields: " << macro << "_FIELDS,\n    properties: "
			<< (object.properties.empty() ? std::string("&[]")
										  : macro + "_PROPERTIES")
			<< ",\n    methods: &[";
		for (std::size_t index = 0; index < object.methods.size(); ++index) {
			if (index != 0u)
				out << ", ";
			out << quoteName(object.methods.at(index).name, true);
		}
		out << "],\n};\n\n";
	}
}

namespace {

//! A WIT member identifier, escaping the words WIT treats as keywords.
std::string witMemberName(std::string_view name)
{
	std::string text = witIdentifier(std::string(name));
	static const char* const reserved[] = { "type", "record", "variant", "enum",
		"flags", "resource", "func", "world", "interface", "package", "use",
		"import", "export", "own", "borrow", "list", "option", "result", "tuple",
		"stream", "future", "async", "static", "constructor" };
	for (const char* word : reserved) {
		if (text == word) {
			text.insert(0, "%");
			break;
		}
	}
	return text;
}

//! The reason WIT cannot express a type, for an emitted note.
std::string witOmission(const ValueType& type)
{
	if (type.kind == ValueKind::Unknown)
		return "an absent type";
	if (type.kind == ValueKind::Ref)
		return "a reference that is not another host object";
	return std::string("`") + valueKindName(type.kind) + "`";
}

//! A type-name spelling of one object, for C++ and Kotlin: `DemoWidgetV1`.
std::string pascalObjectName(const ObjectDescriptor& object)
{
	const std::string name = object.name;
	std::string out;
	bool upper = true;
	for (const char character : name) {
		if (!std::isalnum(static_cast<unsigned char>(character))) {
			upper = true;
			continue;
		}
		out.push_back(upper ? static_cast<char>(
							  std::toupper(static_cast<unsigned char>(character)))
							: character);
		upper = false;
	}
	if (out.empty() || !std::isalpha(static_cast<unsigned char>(out.front())))
		out.insert(0, "Object");
	return out + "V" + std::to_string(object.version);
}

//! The lower-camel spelling of a member name (`do_something` -> `doSomething`).
std::string camelMemberName(std::string_view name)
{
	const std::string snake = objectIdentifier(name);
	std::string out;
	bool upper = false;
	for (const char character : snake) {
		if (character == '_') {
			upper = true;
			continue;
		}
		out.push_back(upper
						? static_cast<char>(
								  std::toupper(static_cast<unsigned char>(character)))
						: character);
		upper = false;
	}
	if (!out.empty())
		out.front() = static_cast<char>(
				std::tolower(static_cast<unsigned char>(out.front())));
	return out;
}

//! The Pascal spelling of a member name, for a setter (`status` -> `Status`).
std::string pascalMemberName(std::string_view name)
{
	std::string out = camelMemberName(name);
	if (!out.empty())
		out.front() = static_cast<char>(
				std::toupper(static_cast<unsigned char>(out.front())));
	return out;
}

} // namespace

void emitCppObjects(std::ostringstream& out, const Schema& schema)
{
	const std::vector<ObjectDescriptor>* objects = objectList(schema);
	if (objects == nullptr)
		return;

	out << "#ifdef __cplusplus\n\n"
		   "/* RAII wrappers over the handle surface (`SO-3`/`SO-4`).\n"
		   "   A wrapper owns one slot, so it is move-only: the destructor releases the\n"
		   "   slot through the `_release` entry point above, and every member forwards\n"
		   "   to an entry point. The slot is host state — a wrapper means nothing in\n"
		   "   another Store or process — and `release()` hands ownership back without\n"
		   "   releasing, which is what a caller wants when passing the slot on. */\n\n";
	for (const ObjectDescriptor& object : *objects) {
		const std::string klass = pascalObjectName(object);
		const std::string type = objectTypeName(object);
		const std::string none = macroPrefix(object) + "_HANDLE_NONE";
		const std::vector<ObjectProperty>& properties = object.properties;

		// The wrapper's own members are taken. A descriptor member that spells one
		// of them is reachable through its C entry point, which keeps the class
		// unambiguous instead of emitting two members with one name.
		const std::set<std::string> reserved = { "handle", "valid", "reset",
			"release" };

		// An accessor is projected as its property, so the standalone method of
		// that name is not emitted a second time: one member, one spelling.
		std::set<std::string> claimed;
		for (const ObjectProperty& property : properties) {
			if (!property.getter.empty())
				claimed.insert(property.getter);
			if (property.mutableProperty)
				claimed.insert(property.setter);
		}

		out << "/* `" << object.name << "` version " << object.version << ". */\n";
		out << "class " << klass << " {\npublic:\n";
		out << "\t//! An empty wrapper: `valid()` is false and nothing is released.\n";
		out << "\t" << klass << "() noexcept : _handle(" << none << ") {}\n";
		out << "\t//! Takes ownership of `handle`; the caller must not release it.\n";
		out << "\texplicit " << klass << "(" << type
			<< "_handle handle) noexcept : _handle(handle) {}\n";
		// A declared constructor is a C++ constructor: no `self`, the result is the
		// slot. When its parameter list would *be* the default or the adopting
		// constructor's, it becomes a static factory with the same name — one call
		// spelled once, and a class that still compiles.
		for (const ObjectConstructor& constructor : object.constructors) {
			const std::string name = constructor.name;
			const std::string entry = type + "_" + objectIdentifier(name);
			std::string args;
			std::string declared;
			for (std::size_t index = 0; index < constructor.params.size(); ++index) {
				if (index != 0) {
					args += ", ";
					declared += ", ";
				}
				const std::string param = camelMemberName(
						constructor.paramName(index));
				args += param;
				declared += objectCType(schema, constructor.params.at(index)) + " "
						+ param;
			}
			const bool adopts = constructor.params.size() == 1u
					&& objectCType(schema, constructor.params.at(0)) == "int64_t";
			if (constructor.params.empty() || adopts) {
				const std::string factory = camelMemberName(name);
				if (reserved.count(factory) != 0u) {
					out << "\t//! `" << name << "` is not wrapped: the wrapper's own"
						   " `" << factory << "` is a member. Call `" << entry
						<< "` directly.\n";
					continue;
				}
				out << "\t//! `" << name << "`, as a static factory: its parameter"
					   " list would be the "
					<< (constructor.params.empty() ? "default" : "adopting")
					<< " constructor's.\n";
				out << "\tstatic " << klass << " " << factory << "(" << declared
					<< ")\n\t{\n\t\treturn " << klass << "(" << entry << "(" << args
					<< "));\n\t}\n";
			} else {
				out << "\t//! `" << name << "`, a declared constructor (a slot of "
					<< macroPrefix(object)
					<< "_HANDLE_NONE leaves `valid()` false).\n";
				out << "\texplicit " << klass << "(" << declared
					<< ")\n\t\t: _handle(" << entry << "(" << args << "))\n\t{\n\t}\n";
			}
		}
		out << "\t//! Releases the slot, if the wrapper still owns one.\n";
		out << "\t~" << klass << "() { reset(); }\n\n";
		out << "\t" << klass << "(const " << klass << "&) = delete;\n";
		out << "\t" << klass << "& operator=(const " << klass << "&) = delete;\n\n";
		out << "\t//! Takes the slot from `other`, leaving it empty.\n";
		out << "\t" << klass << "(" << klass
			<< "&& other) noexcept : _handle(other._handle)\n\t{\n\t\tother._handle = "
			<< none << ";\n\t}\n";
		out << "\t//! Releases the current slot, then takes `other`'s.\n";
		out << "\t" << klass << "& operator=(" << klass
			<< "&& other) noexcept\n\t{\n\t\tif (this != &other) {\n\t\t\treset();\n"
			   "\t\t\t_handle = other._handle;\n\t\t\tother._handle = "
			<< none << ";\n\t\t}\n\t\treturn *this;\n\t}\n\n";
		out << "\t//! True when the wrapper still owns a slot.\n";
		out << "\tbool valid() const noexcept { return _handle != " << none
			<< "; }\n";
		out << "\t//! The same test, so `if (object)` reads naturally.\n";
		out << "\texplicit operator bool() const noexcept { return valid(); }\n";
		out << "\t//! The slot, for a call that takes one.\n";
		out << "\t" << type << "_handle handle() const noexcept { return _handle; }\n\n";
		out << "\t//! Releases the slot now; the destructor then does nothing.\n";
		out << "\tvoid reset() noexcept\n\t{\n\t\tif (_handle != " << none
			<< ") {\n\t\t\t" << type << "_release(_handle);\n\t\t\t_handle = " << none
			<< ";\n\t\t}\n\t}\n\n";
		out << "\t//! Gives up the slot *without* releasing it; the caller then owns it.\n";
		out << "\t" << type << "_handle release() noexcept\n\t{\n\t\tconst " << type
			<< "_handle gave = _handle;\n\t\t_handle = " << none
			<< ";\n\t\treturn gave;\n\t}\n";

		for (const ObjectProperty& property : properties) {
			const std::string name = property.name;
			const std::string member = camelMemberName(name);
			if (reserved.count(member) != 0u) {
				out << "\n\t//! `" << name << "` is not a member: the wrapper's own"
					   " `" << member << "` is. Call `" << type << "_prop_"
					<< objectIdentifier(name) << "_get` directly.\n";
				continue;
			}
			out << "\n\t//! Property `" << name << "` (computed by `"
				<< property.getter << "`).\n";
			out << "\t" << objectCType(schema, property.type) << " " << member
				<< "() const { return " << type << "_prop_"
				<< objectIdentifier(name) << "_get(_handle); }\n";
			if (property.mutableProperty)
				out << "\tvoid set" << pascalMemberName(name) << "("
					<< objectCType(schema, property.type) << " value) { " << type
					<< "_prop_" << objectIdentifier(name)
					<< "_set(_handle, value); }\n";
		}

		for (const ObjectMethod& method : object.methods) {
			const std::string name = method.name;
			if (claimed.count(name) != 0u)
				continue;
			const std::string member = camelMemberName(name);
			if (reserved.count(member) != 0u) {
				out << "\n\t//! `" << name << "` is not a member: the wrapper's own"
					   " `" << member << "` is. Call `" << type << "_"
					<< objectIdentifier(name) << "` directly.\n";
				continue;
			}
			out << "\n\t//! Method `" << name << "`.\n";
			out << "\t"
				<< (method.results.size() == 1u ? cType(method.results.at(0))
											   : std::string("void"))
				<< " " << member << "(";
			for (std::size_t index = 0; index < method.params.size(); ++index) {
				if (index != 0)
					out << ", ";
				out << objectCType(schema, method.params.at(index)) << " "
					<< camelMemberName(method.paramName(index));
			}
			if (method.results.size() > 1u)
				out << (method.params.empty() ? "" : ", ") << "void* results";
			out << ")\n\t{\n\t\t"
				<< (method.results.size() == 1u ? "return " : "") << type << "_"
				<< objectIdentifier(name) << "(_handle";
			for (std::size_t index = 0; index < method.params.size(); ++index)
				out << ", " << camelMemberName(method.paramName(index));
			if (method.results.size() > 1u)
				out << ", results";
			out << ");\n\t}\n";
		}

		out << "\nprivate:\n\t" << type << "_handle _handle;\n};\n\n";
	}
	out << "#endif /* __cplusplus */\n\n";
}

void emitKotlinObjects(std::ostringstream& out, const Schema& schema)
{
	const std::vector<ObjectDescriptor>* objects = objectList(schema);
	if (objects == nullptr)
		return;

	out << "\n// Host-registered objects (wasmjit.binding/2). The wrapper holds the `i64`\n"
		   "// slot; the host's table owns the object, so `close()` is the one release and\n"
		   "// `use { }` is the lifetime policy — the same contract the component model\n"
		   "// spells `resource.drop` (docs/proposals/SYNTHETIC_OBJECTS.md A.14.4).\n\n";
	for (const ObjectDescriptor& object : *objects) {
		const std::string wrapper = pascalObjectName(object);
		const std::string type = objectTypeName(object);
		out << "// `" << object.name << "` version " << object.version << ".\n";
		out << "class " << wrapper << "(private val handle: Long) : AutoCloseable {\n"
			   "    private var released = false\n"
			   "\n"
			   "    /** The host's slot for this object. */\n"
			   "    val raw: Long get() = handle\n"
			   "\n"
			   "    /** Releases the slot once; a second call is a no-op. */\n"
			   "    override fun close() {\n"
			   "        if (released) return\n"
			   "        released = true\n"
			   "        "
			<< type << "_release(handle)\n"
					   "    }\n"
					   "\n"
					   "    companion object {\n";
		for (const ObjectConstructor& constructor : object.constructors) {
			std::string declared;
			std::string args;
			std::string omission;
			for (std::size_t index = 0; index < constructor.params.size(); ++index) {
				const std::optional<std::string> kotlin =
						objectKotlinType(constructor.params.at(index));
				if (!kotlin) {
					omission = "a non-scalar parameter";
					break;
				}
				if (index != 0u) {
					declared += ", ";
					args += ", ";
				}
				const std::string param = camelMemberName(
						constructor.paramName(index));
				declared += param + ": " + *kotlin;
				args += param;
			}
			if (!omission.empty()) {
				out << "        // `" << constructor.name << "`: " << omission
					<< " is not expressible here; call `" << type << "_"
					<< objectIdentifier(constructor.name) << "` directly.\n";
				continue;
			}
			out << "        /** `" << constructor.name
				<< "`: a declared constructor. */\n";
			out << "        fun " << camelMemberName(constructor.name) << "("
				<< declared << "): " << wrapper << " =\n"
				<< "            " << wrapper << "(" << type << "_"
				<< objectIdentifier(constructor.name) << "(" << args << "))\n\n";
		}
		out << "        /** The null slot. */\n"
			   "        const val HANDLE_NONE: Long = 0\n"
			   "    }\n"
			   "}\n"
			   "\n"
			   "// Provided by the host: `HostHandleTable::release`, which is idempotent.\n"
			   "external fun " << type << "_release(handle: Long)\n\n";
	}
}

void emitWitObjects(std::ostringstream& out, const Schema& schema)
{
	const std::vector<ObjectDescriptor>* objects = objectList(schema);
	if (objects == nullptr)
		return;

	for (const ObjectDescriptor& object : *objects) {
		const std::string& name = object.name;
		const std::uint32_t version = object.version;
		const std::uint32_t size = object.size;
		// An accessor is projected as part of its property, so the standalone
		// method of that name is not emitted again: one member, one spelling.
		std::set<std::string> claimed;
		for (const ObjectProperty& property : object.properties) {
			if (!property.getter.empty())
				claimed.insert(property.getter);
			if (property.mutableProperty)
				claimed.insert(property.setter);
		}

		out << "  /// Host-registered object `" << name << "`, descriptor version "
			<< version << ".\n"
			<< "  /// Canonical size " << size
			<< " bytes; the offsets are host metadata, not a Wasm ABI.\n";
		if (!object.parent.empty() || !object.interfaces.empty())
			out << "  /// Parent and interfaces are descriptor metadata; WIT resources"
				   " have no inheritance.\n";

		std::ostringstream members;
		for (const ObjectField& field : object.fields) {
			const std::string fieldName = field.name;
			const ValueType& type = field.type;
			const std::optional<std::string> wit = witType(schema, type);
			if (!wit) {
				members << "    /// " << witMemberName(fieldName) << ": "
					<< witOmission(type) << " is not expressible in WIT; omitted.\n";
				continue;
			}
			if (field.isReference) {
				// WIT forbids returning a `borrow<T>` (a borrowed handle cannot
				// escape the call), so a reference field has no getter; a mutable
				// one still gets the setter WIT can express.
				members << "    /// " << witMemberName(fieldName)
					<< ": a reference field has no WIT getter (a borrow cannot be"
					" returned).\n";
				if (field.mutableField)
					members << "    set-" << witMemberName(fieldName)
						<< ": func(value: " << *wit << ");\n";
				continue;
			}
			members << "    " << witMemberName(fieldName) << ": func() -> " << *wit
				<< ";\n";
			if (field.mutableField)
				members << "    set-" << witMemberName(fieldName)
					<< ": func(value: " << *wit << ");\n";
		}
		for (const ObjectProperty& property : object.properties) {
			const std::string propName = property.name;
			const std::optional<std::string> wit = witType(schema, property.type);
			if (!wit) {
				members << "    /// " << witMemberName(propName)
					<< ": a property of " << witOmission(property.type)
					<< " is not expressible in WIT; omitted.\n";
				continue;
			}
			if (property.type.kind == ValueKind::Ref)
				members << "    /// " << witMemberName(propName)
					<< ": a property of a reference has no WIT getter (a borrow"
					   " cannot be returned).\n";
			else
				members << "    /// Property `" << propName << "`: computed by `"
					<< property.getter
					<< "`, not a stored slot.\n"
					<< "    " << witMemberName(propName) << ": func() -> " << *wit
					<< ";\n";
			if (property.mutableProperty)
				members << "    set-" << witMemberName(propName) << ": func(value: "
					<< *wit << ");\n";
		}
		for (const ObjectMethod& method : object.methods) {
			const std::string methodName = method.name;
			if (claimed.count(methodName) != 0u)
				continue;
			std::vector<std::string> paramTypes;
			std::string omission;
			for (const ValueType& param : method.params) {
				const std::optional<std::string> wit = witType(schema, param);
				if (!wit) {
					omission = witOmission(param) + " parameter";
					break;
				}
				paramTypes.push_back(*wit);
			}
			std::optional<std::string> result;
			if (omission.empty() && method.results.size() > 1u)
				omission = "more than one result";
			if (omission.empty() && method.results.size() == 1u) {
				result = witType(schema, method.results.at(0));
				if (!result)
					omission = witOmission(method.results.at(0)) + " result";
			}
			if (!omission.empty()) {
				members << "    /// " << witMemberName(methodName) << ": " << omission
					<< " is not expressible in WIT; omitted.\n";
				continue;
			}
			members << "    " << witMemberName(methodName) << ": func(";
			for (std::size_t index = 0; index < paramTypes.size(); ++index) {
				if (index != 0u)
					members << ", ";
				members << witIdentifier(method.paramName(index)) << ": "
					<< paramTypes[index];
			}
			members << ")";
			if (result)
				members << " -> " << *result;
			members << ";\n";
		}

		// A constructor is a WIT `constructor`: the canonical ABI's own spelling of
		// "this call makes one of these". WIT names every constructor
		// `[constructor]r` — the signature is not part of the name — so a resource
		// carries **one**, and the first expressible one wins; the rest are noted.
		bool constructorEmitted = false;
		for (const ObjectConstructor& constructor : object.constructors) {
			std::vector<std::string> types;
			std::string omission;
			for (const ValueType& param : constructor.params) {
				const std::optional<std::string> wit = witType(schema, param);
				if (!wit) {
					omission = witOmission(param) + " parameter";
					break;
				}
				types.push_back(*wit);
			}
			if (omission.empty() && constructorEmitted)
				omission = "a second constructor";
			if (!omission.empty()) {
				members << "    /// constructor `" << constructor.name << "`: "
					<< omission << " is not expressible in WIT"
					<< (omission == "a second constructor"
									? " (one `[constructor]` per resource); call `"
									: "; call `")
					<< objectTypeName(object) << "_"
					<< objectIdentifier(constructor.name) << "` instead.\n";
				continue;
			}
			members << "    constructor(";
			for (std::size_t index = 0; index < types.size(); ++index) {
				if (index != 0u)
					members << ", ";
				members << witIdentifier(constructor.paramName(index)) << ": "
					<< types[index];
			}
			members << ");\n";
			constructorEmitted = true;
		}

		if (members.str().empty())
			out << "  resource " << witObjectName(object) << ";\n";
		else
			out << "  resource " << witObjectName(object) << " {\n"
				<< members.str() << "  }\n";
	}
}

} // namespace WASM::Bindings::detail
