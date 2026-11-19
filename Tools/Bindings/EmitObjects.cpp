#include "EmitObjects.hpp"

#include "EmitShared.hpp"

#include <cctype>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace WASM::Bindings::detail {

using Json = nlohmann::ordered_json;

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
const Json* objectArray(const Json& schema)
{
	if (!schema.contains("objects") || !schema.at("objects").is_array()
			|| schema.at("objects").empty())
		return nullptr;
	return &schema.at("objects");
}

//! The current version of a named descriptor, or null.
//!
//! A name is a lookup key, not an identity (A.14.2): when several versions are
//! declared, the highest one is what the name means.
const Json* namedObject(const Json& schema, std::string_view name)
{
	if (!schema.contains("objects") || !schema.at("objects").is_array())
		return nullptr;
	const Json* found = nullptr;
	for (const Json& object : schema.at("objects")) {
		if (object.value("name", "") != name)
			continue;
		if (found == nullptr
				|| object.value("version", 0u) > found->value("version", 0u))
			found = &object;
	}
	return found;
}

//! The macro prefix of an object: `WASMJIT_OBJECT_DEMO_WIDGET_V1`.
std::string macroPrefix(const Json& object)
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
std::string witObjectName(const Json& object)
{
	return witIdentifier(object.value("name", "")) + "-v"
			+ std::to_string(object.value("version", 1u));
}

//! The C spelling of a descriptor type, resolving a defined reference to the
//! object it names.
std::string objectCType(const Json& schema, const Json& type)
{
	if (type.is_object() && type.value("type", "") == "ref") {
		if (type.value("heap", "") == "defined") {
			if (const Json* target = namedObject(schema, type.value("name", "")))
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
std::optional<std::string> objectKotlinType(const Json& type)
{
	if (!type.is_object() || type.value("type", "") == "ref")
		return std::nullopt;
	const std::string kind = type.value("type", "");
	if (type.value("packed", false))
		return kind == "i8" ? "Byte" : "Short";
	if (kind == "i32" || kind == "s32") return "Int";
	if (kind == "u32") return "UInt";
	if (kind == "i64" || kind == "s64") return "Long";
	if (kind == "u64") return "ULong";
	if (kind == "f32") return "Float";
	if (kind == "f64") return "Double";
	return std::nullopt;
}

//! The Rust spelling of a descriptor type, with the same resolution as C.
std::string objectRustType(const Json& schema, const Json& type)
{
	if (type.is_object() && type.value("type", "") == "ref"
			&& type.value("heap", "") == "defined") {
		if (const Json* target = namedObject(schema, type.value("name", "")))
			return "*mut " + objectTypeName(*target);
	}
	return rustType(type);
}

//! The descriptor spelling of a type, for the metadata tables (`i32`,
//! `ref extern`, `ref demo.Node`).
std::string descriptorTypeName(const Json& type)
{
	if (!type.is_object())
		return "unknown";
	std::string out = type.value("type", std::string("unknown"));
	if (out == "ref") {
		out += " ";
		out += type.value("heap", std::string("unknown"));
		const std::string name = type.value("name", std::string());
		if (!name.empty())
			out += " " + name;
	}
	if (type.value("packed", false))
		out += " (packed)";
	return out;
}

//! The WIT spelling of one descriptor type, or nullopt when WIT cannot say it.
std::optional<std::string> witType(const Json& schema, const Json& type)
{
	if (!type.is_object())
		return std::nullopt;
	if (type.value("type", "") == "ref") {
		// A reference to another host object is a borrowed handle; every other
		// reference (externref, anyref, ...) has no WIT spelling.
		if (type.value("heap", "") != "defined")
			return std::nullopt;
		const Json* target = namedObject(schema, type.value("name", ""));
		if (target == nullptr)
			return std::nullopt;
		return "borrow<" + witObjectName(*target) + ">";
	}
	const std::string kind = type.value("type", "");
	if (kind == "i32") return "s32";
	if (kind == "i64") return "s64";
	if (kind == "f32") return "f32";
	if (kind == "f64") return "f64";
	if (type.value("packed", false)) {
		if (kind == "i8") return "s8";
		if (kind == "i16") return "s16";
	}
	return std::nullopt;
}

} // namespace

std::string objectIdentifier(std::string_view name)
{
	return sanitize(lowerCopy(std::string(name)));
}

std::string objectTypeName(const Json& object)
{
	return "wasmjit_object_" + objectIdentifier(object.value("name", ""))
			+ "_v" + std::to_string(object.value("version", 1u));
}

void emitCObjects(std::ostringstream& out, const Json& schema)
{
	const Json* objects = objectArray(schema);
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
	const Json emptyArray = Json::array();
	for (const Json& object : *objects) {
		const std::string type = objectTypeName(object);
		out << "typedef struct " << type << " " << type << ";\n";
	}
	out << "\n";

	for (const Json& object : *objects) {
		const std::string type = objectTypeName(object);
		const std::string macro = macroPrefix(object);
		const std::string name = object.value("name", "");
		const std::uint32_t version = object.value("version", 1u);
		const std::uint32_t size = object.value("size", 0u);
		const Json& fields =
				object.contains("fields") ? object.at("fields") : emptyArray;
		const Json& properties =
				object.contains("properties") ? object.at("properties") : emptyArray;
		const Json& methods =
				object.contains("methods") ? object.at("methods") : emptyArray;
		const Json& constructors =
				object.contains("constructors") ? object.at("constructors")
											   : emptyArray;

		out << "/* " << name << " version " << version;
		if (object.contains("parent"))
			out << ", parent " << object.at("parent").get<std::string>();
		if (object.contains("interfaces") && object.at("interfaces").is_array()
				&& !object.at("interfaces").empty()) {
			out << ", interfaces ";
			bool first = true;
			for (const Json& entry : object.at("interfaces")) {
				if (!first)
					out << ", ";
				first = false;
				out << entry.get<std::string>();
			}
		}
		out << ". */\n";
		out << "#define " << macro << "_VERSION " << version << "u\n";
		out << "#define " << macro << "_SIZE " << size << "u\n";
		for (const Json& field : fields) {
			out << "#define " << macro << "_FIELD_"
				<< upperCopy(objectIdentifier(field.value("name", ""))) << " "
				<< field.value("offset", 0u) << "u\n";
		}
		for (const Json& method : methods) {
			const Json& params =
					method.contains("params") ? method.at("params") : emptyArray;
			const Json& results =
					method.contains("results") ? method.at("results") : emptyArray;
			out << "typedef ";
			if (results.size() == 1u)
				out << cType(results.at(0));
			else
				out << "void";
			out << " (*" << type << "_"
				<< objectIdentifier(method.value("name", "")) << "_fn)("
				<< type << "* self";
			for (const Json& param : params)
				out << ", " << objectCType(schema, param);
			if (results.size() > 1u)
				out << ", void* results";
			out << ");\n";
		}
		if (!properties.empty()) {
			out << "\n/* Properties: a value reached through a function, never a slot.\n"
				   "   The accessor names are the instance's export names; a host\n"
				   "   binds one trampoline per name and reads the property through\n"
				   "   it, so a getter may compute or cascade rather than load. */\n";
			for (const Json& property : properties) {
				const std::string propName = property.value("name", "");
				const std::string propIdent = objectIdentifier(propName);
				const std::string propMacro = macro + "_PROP_" + upperCopy(propIdent);
				const Json& propType =
						property.contains("type") ? property.at("type") : Json{};
				out << "#define " << propMacro << "_GET "
					<< quoteName(property.value("get", ""), false) << "\n";
				if (property.contains("set"))
					out << "#define " << propMacro << "_SET "
						<< quoteName(property.value("set", ""), false) << "\n";
				out << "typedef " << objectCType(schema, propType) << " (*" << type
					<< "_prop_" << propIdent << "_get_fn)(" << type << "* self);\n";
				if (property.contains("set"))
					out << "typedef void (*" << type << "_prop_" << propIdent
						<< "_set_fn)(" << type << "* self, "
						<< objectCType(schema, propType) << " value);\n";
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
		for (const Json& constructor : constructors) {
			const Json& params =
					constructor.contains("params") ? constructor.at("params")
												   : emptyArray;
			out << "typedef int64_t (*" << type << "_"
				<< objectIdentifier(constructor.value("name", "")) << "_h_fn)(";
			if (params.empty())
				out << "void";
			for (std::size_t index = 0; index < params.size(); ++index) {
				if (index != 0)
					out << ", ";
				out << objectCType(schema, params[index]);
			}
			out << ");\n";
		}
		for (const Json& method : methods) {
			const Json& params =
					method.contains("params") ? method.at("params") : emptyArray;
			const Json& results =
					method.contains("results") ? method.at("results") : emptyArray;
			out << "typedef ";
			if (results.size() == 1u)
				out << cType(results.at(0));
			else
				out << "void";
			out << " (*" << type << "_"
				<< objectIdentifier(method.value("name", "")) << "_h_fn)(int64_t self";
			for (const Json& param : params)
				out << ", " << objectCType(schema, param);
			if (results.size() > 1u)
				out << ", void* results";
			out << ");\n";
		}
		for (const Json& property : properties) {
			const std::string propIdent =
					objectIdentifier(property.value("name", ""));
			const Json& propType =
					property.contains("type") ? property.at("type") : Json{};
			out << "typedef " << objectCType(schema, propType) << " (*" << type
				<< "_prop_" << propIdent << "_get_h_fn)(int64_t self);\n";
			if (property.contains("set"))
				out << "typedef void (*" << type << "_prop_" << propIdent
					<< "_set_h_fn)(int64_t self, " << objectCType(schema, propType)
					<< " value);\n";
		}
		// The entry points those typedefs describe. The tool declares them; the
		// embedder implements one line each — resolve the slot, then forward to
		// the typed accessor — so the operation keeps a single implementation.
		out << "\n/* The handle entry points: one per signature above, plus the release.\n"
			   "   Implement each as `resolve the slot, then forward`; the generated\n"
			   "   C++ wrapper below calls them and owns the slot for you. */\n";
		out << "void " << type << "_release(" << type << "_handle self);\n";
		for (const Json& constructor : constructors) {
			const Json& params =
					constructor.contains("params") ? constructor.at("params")
												   : emptyArray;
			out << type << "_handle " << type << "_"
				<< objectIdentifier(constructor.value("name", "")) << "(";
			if (params.empty())
				out << "void";
			for (std::size_t index = 0; index < params.size(); ++index) {
				if (index != 0)
					out << ", ";
				out << objectCType(schema, params[index]) << " p" << index;
			}
			out << ");\n";
		}
		for (const Json& method : methods) {
			const Json& params =
					method.contains("params") ? method.at("params") : emptyArray;
			const Json& results =
					method.contains("results") ? method.at("results") : emptyArray;
			out << (results.size() == 1u ? cType(results.at(0)) : "void") << " "
				<< type << "_" << objectIdentifier(method.value("name", "")) << "("
				<< type << "_handle self";
			for (const Json& param : params)
				out << ", " << objectCType(schema, param);
			if (results.size() > 1u)
				out << ", void* results";
			out << ");\n";
		}
		for (const Json& property : properties) {
			const std::string propIdent =
					objectIdentifier(property.value("name", ""));
			const Json& propType =
					property.contains("type") ? property.at("type") : Json{};
			out << objectCType(schema, propType) << " " << type << "_prop_"
				<< propIdent << "_get(" << type << "_handle self);\n";
			if (property.contains("set"))
				out << "void " << type << "_prop_" << propIdent << "_set(" << type
					<< "_handle self, " << objectCType(schema, propType)
					<< " value);\n";
		}
		out << "\n";

		if (!fields.empty()) {
			out << "static const wasmjit_object_meta_field " << type
				<< "_fields[] = {\n";
			for (const Json& field : fields) {
				out << "    { " << quoteName(field.value("name", ""), false) << ", "
					<< field.value("offset", 0u) << "u, "
					<< field.value("bytes", 0u) << "u, "
					<< (field.value("reference", false) ? 1 : 0) << "u, "
					<< (field.value("mutable", false) ? 1 : 0) << "u },\n";
			}
			out << "};\n";
		}
		if (!methods.empty()) {
			out << "static const char* const " << type << "_methods[] = {";
			for (std::size_t index = 0; index < methods.size(); ++index) {
				if (index != 0u)
					out << ", ";
				out << quoteName(methods.at(index).value("name", ""), false);
			}
			out << "};\n";
		}
		if (!properties.empty()) {
			out << "static const wasmjit_object_property_meta " << type
				<< "_properties[] = {\n";
			for (const Json& property : properties) {
				const Json& propType =
						property.contains("type") ? property.at("type") : Json{};
				out << "    { " << quoteName(property.value("name", ""), false)
					<< ", " << quoteName(descriptorTypeName(propType), false)
					<< ", " << quoteName(property.value("get", ""), false) << ", ";
				if (property.contains("set"))
					out << quoteName(property.value("set", ""), false);
				else
					out << "NULL";
				out << ", " << (property.contains("set") ? 1 : 0) << "u },\n";
			}
			out << "};\n";
		}
		out << "static const wasmjit_object_meta " << type << "_meta = {\n"
			<< "    " << quoteName(name, false) << ", " << version << "u, "
			<< size << "u, ";
		if (object.contains("parent"))
			out << quoteName(object.at("parent").get<std::string>(), false);
		else
			out << "NULL";
		out << ", " << fields.size() << "u, "
			<< (fields.empty() ? std::string("NULL") : type + "_fields") << ", "
			<< properties.size() << "u, "
			<< (properties.empty() ? std::string("NULL") : type + "_properties")
			<< ", " << methods.size() << "u, "
			<< (methods.empty() ? std::string("NULL") : type + "_methods")
			<< "\n};\n\n";
	}
}

void emitRustObjects(std::ostringstream& out, const Json& schema)
{
	const Json* objects = objectArray(schema);
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

	const Json emptyArray = Json::array();
	for (const Json& object : *objects) {
		const std::string type = objectTypeName(object);
		const std::string macro = macroPrefix(object);
		const std::string name = object.value("name", "");
		const std::uint32_t version = object.value("version", 1u);
		const std::uint32_t size = object.value("size", 0u);
		const Json& fields =
				object.contains("fields") ? object.at("fields") : emptyArray;
		const Json& properties =
				object.contains("properties") ? object.at("properties") : emptyArray;
		const Json& methods =
				object.contains("methods") ? object.at("methods") : emptyArray;

		out << "// " << name << " version " << version;
		if (object.contains("parent"))
			out << ", parent " << object.at("parent").get<std::string>();
		if (object.contains("interfaces") && object.at("interfaces").is_array()
				&& !object.at("interfaces").empty()) {
			out << ", interfaces ";
			bool first = true;
			for (const Json& entry : object.at("interfaces")) {
				if (!first)
					out << ", ";
				first = false;
				out << entry.get<std::string>();
			}
		}
		out << ".\n";
		out << "#[repr(C)]\npub struct " << type << " { _private: [u8; 0] }\n";
		out << "pub const " << macro << "_VERSION: u32 = " << version << ";\n";
		out << "pub const " << macro << "_SIZE: u32 = " << size << ";\n";
		for (const Json& field : fields) {
			out << "pub const " << macro << "_FIELD_"
				<< upperCopy(objectIdentifier(field.value("name", ""))) << ": u32 = "
				<< field.value("offset", 0u) << ";\n";
		}
		for (const Json& method : methods) {
			const Json& params =
					method.contains("params") ? method.at("params") : emptyArray;
			const Json& results =
					method.contains("results") ? method.at("results") : emptyArray;
			out << "pub type " << type << "_"
				<< objectIdentifier(method.value("name", ""))
				<< "_fn = unsafe extern \"C\" fn(*mut " << type;
			for (const Json& param : params)
				out << ", " << objectRustType(schema, param);
			if (results.size() > 1u)
				out << ", result: *mut core::ffi::c_void";
			out << ")";
			if (results.size() == 1u)
				out << " -> " << objectRustType(schema, results.at(0));
			out << ";\n";
		}
		if (!properties.empty()) {
			out << "\n";
			for (const Json& property : properties) {
				const std::string propIdent =
						objectIdentifier(property.value("name", ""));
				const std::string propMacro = macro + "_PROP_" + upperCopy(propIdent);
				const Json& propType =
						property.contains("type") ? property.at("type") : Json{};
				out << "pub const " << propMacro << "_GET: &str = "
					<< quoteName(property.value("get", ""), true) << ";\n";
				if (property.contains("set"))
					out << "pub const " << propMacro << "_SET: &str = "
						<< quoteName(property.value("set", ""), true) << ";\n";
				out << "pub type " << type << "_prop_" << propIdent
					<< "_get_fn = unsafe extern \"C\" fn(*mut " << type << ") -> "
					<< objectRustType(schema, propType) << ";\n";
				if (property.contains("set"))
					out << "pub type " << type << "_prop_" << propIdent
						<< "_set_fn = unsafe extern \"C\" fn(*mut " << type << ", "
						<< objectRustType(schema, propType) << ");\n";
			}
		}
		// The handle surface: the same signatures over the `i64` a non-GC
		// consumer carries, so one operation keeps one implementation.
		out << "\n// The handle surface (A.14.4): the `i64` slot a non-GC consumer\n"
			   "// carries. Resolve the slot, then forward to the typed accessor.\n";
		out << "pub type " << type << "_handle = i64;\n";
		out << "pub const " << macro << "_HANDLE_NONE: i64 = 0;\n";
		for (const Json& method : methods) {
			const Json& params =
					method.contains("params") ? method.at("params") : emptyArray;
			const Json& results =
					method.contains("results") ? method.at("results") : emptyArray;
			out << "pub type " << type << "_"
				<< objectIdentifier(method.value("name", ""))
				<< "_h_fn = unsafe extern \"C\" fn(i64";
			for (const Json& param : params)
				out << ", " << objectRustType(schema, param);
			if (results.size() > 1u)
				out << ", result: *mut core::ffi::c_void";
			out << ")";
			if (results.size() == 1u)
				out << " -> " << objectRustType(schema, results.at(0));
			out << ";\n";
		}
		// A constructor has no `self` and returns the slot: one alias per declared
		// constructor, mirroring the C entry points.
		for (const Json& constructor :
				(object.contains("constructors") ? object.at("constructors")
												 : emptyArray)) {
			const Json& params =
					constructor.contains("params") ? constructor.at("params")
												   : emptyArray;
			out << "pub type " << type << "_"
				<< objectIdentifier(constructor.value("name", ""))
				<< "_h_fn = unsafe extern \"C\" fn(";
			for (std::size_t index = 0; index < params.size(); ++index) {
				if (index != 0)
					out << ", ";
				out << objectRustType(schema, params[index]);
			}
			out << ") -> i64;\n";
		}
		for (const Json& property : properties) {
			const std::string propIdent =
					objectIdentifier(property.value("name", ""));
			const Json& propType =
					property.contains("type") ? property.at("type") : Json{};
			out << "pub type " << type << "_prop_" << propIdent
				<< "_get_h_fn = unsafe extern \"C\" fn(i64) -> "
				<< objectRustType(schema, propType) << ";\n";
			if (property.contains("set"))
				out << "pub type " << type << "_prop_" << propIdent
					<< "_set_h_fn = unsafe extern \"C\" fn(i64, "
					<< objectRustType(schema, propType) << ");\n";
		}
		out << "\npub static " << macro
			<< "_FIELDS: &[wasmjit_object_meta_field] = &[\n";
		for (const Json& field : fields) {
			out << "    wasmjit_object_meta_field { name: "
				<< quoteName(field.value("name", ""), true)
				<< ", offset: " << field.value("offset", 0u)
				<< ", bytes: " << field.value("bytes", 0u)
				<< ", is_reference: "
				<< (field.value("reference", false) ? "true" : "false")
				<< ", is_mutable: "
				<< (field.value("mutable", false) ? "true" : "false") << " },\n";
		}
		out << "];\n";
		if (!properties.empty()) {
			out << "pub static " << macro
				<< "_PROPERTIES: &[wasmjit_object_property_meta] = &[\n";
			for (const Json& property : properties) {
				const Json& propType =
						property.contains("type") ? property.at("type") : Json{};
				out << "    wasmjit_object_property_meta { name: "
					<< quoteName(property.value("name", ""), true)
					<< ", type_name: "
					<< quoteName(descriptorTypeName(propType), true)
					<< ", get_export: " << quoteName(property.value("get", ""), true)
					<< ", set_export: ";
				if (property.contains("set"))
					out << "Some(" << quoteName(property.value("set", ""), true)
						<< ")";
				else
					out << "None";
				out << ", is_mutable: "
					<< (property.contains("set") ? "true" : "false") << " },\n";
			}
			out << "];\n";
		}
		out << "pub static " << macro
			<< ": wasmjit_object_meta = wasmjit_object_meta {\n"
			<< "    name: " << quoteName(name, true) << ", version: " << version
			<< ", size: " << size << ", parent: ";
		if (object.contains("parent"))
			out << "Some("
				<< quoteName(object.at("parent").get<std::string>(), true) << ")";
		else
			out << "None";
		out << ",\n    fields: " << macro << "_FIELDS,\n    properties: "
			<< (properties.empty() ? std::string("&[]") : macro + "_PROPERTIES")
			<< ",\n    methods: &[";
		for (std::size_t index = 0; index < methods.size(); ++index) {
			if (index != 0u)
				out << ", ";
			out << quoteName(methods.at(index).value("name", ""), true);
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
std::string witOmission(const Json& type)
{
	if (!type.is_object())
		return "an absent type";
	if (type.value("type", "") == "ref")
		return "a reference that is not another host object";
	return std::string("`") + type.value("type", "unknown") + "`";
}

//! A type-name spelling of one object, for C++ and Kotlin: `DemoWidgetV1`.
std::string pascalObjectName(const Json& object)
{
	const std::string name = object.value("name", std::string());
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
	return out + "V" + std::to_string(object.value("version", 1u));
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

void emitCppObjects(std::ostringstream& out, const Json& schema)
{
	const Json* objects = objectArray(schema);
	if (objects == nullptr)
		return;
	const Json emptyArray = Json::array();

	out << "#ifdef __cplusplus\n\n"
		   "/* RAII wrappers over the handle surface (`SO-3`/`SO-4`).\n"
		   "   A wrapper owns one slot, so it is move-only: the destructor releases the\n"
		   "   slot through the `_release` entry point above, and every member forwards\n"
		   "   to an entry point. The slot is host state — a wrapper means nothing in\n"
		   "   another Store or process — and `release()` hands ownership back without\n"
		   "   releasing, which is what a caller wants when passing the slot on. */\n\n";
	for (const Json& object : *objects) {
		const std::string klass = pascalObjectName(object);
		const std::string type = objectTypeName(object);
		const std::string none = macroPrefix(object) + "_HANDLE_NONE";
		const Json& properties =
				object.contains("properties") ? object.at("properties") : emptyArray;
		const Json& methods =
				object.contains("methods") ? object.at("methods") : emptyArray;
		const Json& constructors =
				object.contains("constructors") ? object.at("constructors")
											   : emptyArray;

		// The wrapper's own members are taken. A descriptor member that spells one
		// of them is reachable through its C entry point, which keeps the class
		// unambiguous instead of emitting two members with one name.
		const std::set<std::string> reserved = { "handle", "valid", "reset",
			"release" };

		// An accessor is projected as its property, so the standalone method of
		// that name is not emitted a second time: one member, one spelling.
		std::set<std::string> claimed;
		for (const Json& property : properties) {
			if (property.contains("get"))
				claimed.insert(property.at("get").get<std::string>());
			if (property.contains("set"))
				claimed.insert(property.at("set").get<std::string>());
		}

		out << "/* `" << object.value("name", "") << "` version "
			<< object.value("version", 1u) << ". */\n";
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
		for (const Json& constructor : constructors) {
			const Json& params =
					constructor.contains("params") ? constructor.at("params")
												   : emptyArray;
			const std::string name = constructor.value("name", "");
			const std::string entry = type + "_" + objectIdentifier(name);
			std::string args;
			std::string declared;
			for (std::size_t index = 0; index < params.size(); ++index) {
				if (index != 0) {
					args += ", ";
					declared += ", ";
				}
				args += "p" + std::to_string(index);
				declared += objectCType(schema, params[index]) + " p"
						+ std::to_string(index);
			}
			const bool adopts = params.size() == 1u
					&& objectCType(schema, params.at(0)) == "int64_t";
			if (params.empty() || adopts) {
				const std::string factory = camelMemberName(name);
				if (reserved.count(factory) != 0u) {
					out << "\t//! `" << name << "` is not wrapped: the wrapper's own"
						   " `" << factory << "` is a member. Call `" << entry
						<< "` directly.\n";
					continue;
				}
				out << "\t//! `" << name << "`, as a static factory: its parameter"
					   " list would be the "
					<< (params.empty() ? "default" : "adopting")
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

		for (const Json& property : properties) {
			const std::string name = property.value("name", "");
			const std::string member = camelMemberName(name);
			const Json& propType =
					property.contains("type") ? property.at("type") : Json{};
			if (reserved.count(member) != 0u) {
				out << "\n\t//! `" << name << "` is not a member: the wrapper's own"
					   " `" << member << "` is. Call `" << type << "_prop_"
					<< objectIdentifier(name) << "_get` directly.\n";
				continue;
			}
			out << "\n\t//! Property `" << name << "` (computed by `"
				<< property.value("get", "") << "`).\n";
			out << "\t" << objectCType(schema, propType) << " " << member
				<< "() const { return " << type << "_prop_"
				<< objectIdentifier(name) << "_get(_handle); }\n";
			if (property.contains("set"))
				out << "\tvoid set" << pascalMemberName(name) << "("
					<< objectCType(schema, propType) << " value) { " << type
					<< "_prop_" << objectIdentifier(name)
					<< "_set(_handle, value); }\n";
		}

		for (const Json& method : methods) {
			const std::string name = method.value("name", "");
			if (claimed.count(name) != 0u)
				continue;
			const Json& params =
					method.contains("params") ? method.at("params") : emptyArray;
			const Json& results =
					method.contains("results") ? method.at("results") : emptyArray;
			const std::string member = camelMemberName(name);
			if (reserved.count(member) != 0u) {
				out << "\n\t//! `" << name << "` is not a member: the wrapper's own"
					   " `" << member << "` is. Call `" << type << "_"
					<< objectIdentifier(name) << "` directly.\n";
				continue;
			}
			out << "\n\t//! Method `" << name << "`.\n";
			out << "\t" << (results.size() == 1u ? cType(results.at(0)) : "void")
				<< " " << member << "(";
			for (std::size_t index = 0; index < params.size(); ++index) {
				if (index != 0)
					out << ", ";
				out << objectCType(schema, params[index]) << " p" << index;
			}
			if (results.size() > 1u)
				out << (params.empty() ? "" : ", ") << "void* results";
			out << ")\n\t{\n\t\t"
				<< (results.size() == 1u ? "return " : "") << type << "_"
				<< objectIdentifier(name) << "(_handle";
			for (std::size_t index = 0; index < params.size(); ++index)
				out << ", p" << index;
			if (results.size() > 1u)
				out << ", results";
			out << ");\n\t}\n";
		}

		out << "\nprivate:\n\t" << type << "_handle _handle;\n};\n\n";
	}
	out << "#endif /* __cplusplus */\n\n";
}

void emitKotlinObjects(std::ostringstream& out, const Json& schema)
{
	const Json* objects = objectArray(schema);
	if (objects == nullptr)
		return;
	const Json emptyArray = Json::array();

	out << "\n// Host-registered objects (wasmjit.binding/2). The wrapper holds the `i64`\n"
		   "// slot; the host's table owns the object, so `close()` is the one release and\n"
		   "// `use { }` is the lifetime policy — the same contract the component model\n"
		   "// spells `resource.drop` (docs/proposals/SYNTHETIC_OBJECTS.md A.14.4).\n\n";
	for (const Json& object : *objects) {
		const std::string wrapper = pascalObjectName(object);
		const std::string type = objectTypeName(object);
		const Json& constructors =
				object.contains("constructors") ? object.at("constructors")
											   : emptyArray;
		out << "// `" << object.value("name", "") << "` version "
			<< object.value("version", 1u) << ".\n";
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
		for (const Json& constructor : constructors) {
			const Json& params =
					constructor.contains("params") ? constructor.at("params")
												   : emptyArray;
			std::string declared;
			std::string args;
			std::string omission;
			for (std::size_t index = 0; index < params.size(); ++index) {
				const std::optional<std::string> kotlin =
						objectKotlinType(params.at(index));
				if (!kotlin) {
					omission = "a non-scalar parameter";
					break;
				}
				if (index != 0u) {
					declared += ", ";
					args += ", ";
				}
				declared += "p" + std::to_string(index) + ": " + *kotlin;
				args += "p" + std::to_string(index);
			}
			if (!omission.empty()) {
				out << "        // `" << constructor.value("name", "")
					<< "`: " << omission
					<< " is not expressible here; call `" << type << "_"
					<< objectIdentifier(constructor.value("name", ""))
					<< "` directly.\n";
				continue;
			}
			out << "        /** `" << constructor.value("name", "")
				<< "`: a declared constructor. */\n";
			out << "        fun " << camelMemberName(constructor.value("name", ""))
				<< "(" << declared << "): " << wrapper << " =\n"
				<< "            " << wrapper << "(" << type << "_"
				<< objectIdentifier(constructor.value("name", "")) << "(" << args
				<< "))\n\n";
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

void emitWitObjects(std::ostringstream& out, const Json& schema)
{
	const Json* objects = objectArray(schema);
	if (objects == nullptr)
		return;

	const Json emptyArray = Json::array();
	for (const Json& object : *objects) {
		const std::string name = object.value("name", "");
		const std::uint32_t version = object.value("version", 1u);
		const std::uint32_t size = object.value("size", 0u);
		const Json& fields =
				object.contains("fields") ? object.at("fields") : emptyArray;
		const Json& properties =
				object.contains("properties") ? object.at("properties") : emptyArray;
		const Json& methods =
				object.contains("methods") ? object.at("methods") : emptyArray;
		// An accessor is projected as part of its property, so the standalone
		// method of that name is not emitted again: one member, one spelling.
		std::set<std::string> claimed;
		for (const Json& property : properties) {
			if (property.contains("get"))
				claimed.insert(property.value("get", ""));
			if (property.contains("set"))
				claimed.insert(property.value("set", ""));
		}

		out << "  /// Host-registered object `" << name << "`, descriptor version "
			<< version << ".\n"
			<< "  /// Canonical size " << size
			<< " bytes; the offsets are host metadata, not a Wasm ABI.\n";
		if (object.contains("parent") || object.contains("interfaces"))
			out << "  /// Parent and interfaces are descriptor metadata; WIT resources"
				   " have no inheritance.\n";

		std::ostringstream members;
		for (const Json& field : fields) {
			const std::string fieldName = field.value("name", "");
			const Json& type = field.contains("type") ? field.at("type") : Json{};
			const bool isReference =
					type.is_object() && type.value("type", "") == "ref";
			const std::optional<std::string> wit = witType(schema, type);
			if (!wit) {
				members << "    /// " << witMemberName(fieldName) << ": "
					<< witOmission(type) << " is not expressible in WIT; omitted.\n";
				continue;
			}
			if (isReference) {
				// WIT forbids returning a `borrow<T>` (a borrowed handle cannot
				// escape the call), so a reference field has no getter; a mutable
				// one still gets the setter WIT can express.
				members << "    /// " << witMemberName(fieldName)
					<< ": a reference field has no WIT getter (a borrow cannot be"
					" returned).\n";
				if (field.value("mutable", false))
					members << "    set-" << witMemberName(fieldName)
						<< ": func(value: " << *wit << ");\n";
				continue;
			}
			members << "    " << witMemberName(fieldName) << ": func() -> " << *wit
				<< ";\n";
			if (field.value("mutable", false))
				members << "    set-" << witMemberName(fieldName)
					<< ": func(value: " << *wit << ");\n";
		}
		for (const Json& property : properties) {
			const std::string propName = property.value("name", "");
			const Json& propType =
					property.contains("type") ? property.at("type") : Json{};
			const std::optional<std::string> wit = witType(schema, propType);
			const bool isReference =
					propType.is_object() && propType.value("type", "") == "ref";
			if (!wit) {
				members << "    /// " << witMemberName(propName)
					<< ": a property of " << witOmission(propType)
					<< " is not expressible in WIT; omitted.\n";
				continue;
			}
			if (isReference)
				members << "    /// " << witMemberName(propName)
					<< ": a property of a reference has no WIT getter (a borrow"
					   " cannot be returned).\n";
			else
				members << "    /// Property `" << propName << "`: computed by `"
					<< property.value("get", "")
					<< "`, not a stored slot.\n"
					<< "    " << witMemberName(propName) << ": func() -> " << *wit
					<< ";\n";
			if (property.contains("set"))
				members << "    set-" << witMemberName(propName) << ": func(value: "
					<< *wit << ");\n";
		}
		for (const Json& method : methods) {
			const Json& params =
					method.contains("params") ? method.at("params") : emptyArray;
			const Json& results =
					method.contains("results") ? method.at("results") : emptyArray;
			const std::string methodName = method.value("name", "");
			if (claimed.count(methodName) != 0u)
				continue;
			std::vector<std::string> paramTypes;
			std::string omission;
			for (const Json& param : params) {
				const std::optional<std::string> wit = witType(schema, param);
				if (!wit) {
					omission = witOmission(param) + " parameter";
					break;
				}
				paramTypes.push_back(*wit);
			}
			std::optional<std::string> result;
			if (omission.empty() && results.size() > 1u)
				omission = "more than one result";
			if (omission.empty() && results.size() == 1u) {
				result = witType(schema, results.at(0));
				if (!result)
					omission = witOmission(results.at(0)) + " result";
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
				members << "p" << index << ": " << paramTypes[index];
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
		for (const Json& constructor :
				(object.contains("constructors") ? object.at("constructors")
												 : emptyArray)) {
			const Json& params =
					constructor.contains("params") ? constructor.at("params")
												   : emptyArray;
			std::vector<std::string> types;
			std::string omission;
			for (const Json& param : params) {
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
				members << "    /// constructor `" << constructor.value("name", "")
					<< "`: " << omission << " is not expressible in WIT"
					<< (omission == "a second constructor"
									? " (one `[constructor]` per resource); call `"
									: "; call `")
					<< objectTypeName(object) << "_"
					<< objectIdentifier(constructor.value("name", ""))
					<< "` instead.\n";
				continue;
			}
			members << "    constructor(";
			for (std::size_t index = 0; index < types.size(); ++index) {
				if (index != 0u)
					members << ", ";
				members << "p" << index << ": " << types[index];
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
