#include "BindingSchema.hpp"
#include "EmitInternal.hpp"
#include "EmitShared.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace WASM::Bindings {

using namespace detail;
using Json = nlohmann::ordered_json;

std::string emitRustModule(const nlohmann::ordered_json& input)
{
	Json schema = input;
	dedupeJsonSchema(schema);
	std::set<std::string> refs;
	collectSchemaRefs(schema, refs);

	std::ostringstream out;
	out << "#![allow(non_camel_case_types)]\n";
	out << "#![allow(non_snake_case)]\n\n";
	out << "// Generated from the " << schema.at("schema").get<std::string>()
		<< " " << schema.at("direction").get<std::string>()
		<< " schema. Do not edit.\n\n";
	out << "// A v128 crosses the C boundary as two scalar words, low first: pass lo and hi\n"
		   "// as two u64 parameters, never as this struct.\n";
	out << "#[repr(C)]\npub struct wasm_v128 { pub bytes: [u8; 16] }\n";
	for (const std::string& ref : refs)
		out << "#[repr(C)]\npub struct " << ref << " { _private: [u8; 0] }\n";
	out << "\n// Reference types are opaque. Reference parameters are borrowed unless\n"
		   "// the adapter says otherwise; reference results carry an explicit\n"
		   "// ownership note. This module defines no layout and no TypeId.\n\n";

	for (const Json& entry : schema.at("exports")) {
		if (entry.value("kind", "") != "function")
			continue;
		const std::string exportName = entry.at("name").get<std::string>();
		const std::string name = "wasmjit_" + sanitize(exportName);
		out << "// H2G export: " << exportComment(entry) << "\n";
		out << "pub type " << name << "_t = unsafe extern \"C\" fn(";
		emitRustParams(out, entry, signatureResults(entry).size() > 1u);
		out << ")";
		emitRustReturn(out, signatureResults(entry));
		out << ";\n\n";

		// A type alias alone cannot be implemented as a Wasm export. Emit a
		// compilable skeleton under the real export name; the developer fills
		// in the body.
		out << "#[export_name = " << quoteName(exportName, true) << "]\n";
		out << "pub extern \"C\" fn " << name << "_impl(";
		emitRustParams(out, entry, signatureResults(entry).size() > 1u);
		out << ")";
		emitRustReturn(out, signatureResults(entry));
		out << " {\n    todo!("
			<< quoteName("implement " + exportName, true) << ");\n}\n\n";
	}

	// Group imports by Wasm import module: Rust requires the module annotation
	// on the extern block and the field name on each declaration.
	std::map<std::string, std::vector<const Json*>> importsByModule;
	for (const Json& entry : schema.at("imports")) {
		if (entry.value("kind", "") != "function")
			continue;
		importsByModule[importModule(entry)].push_back(&entry);
	}
	for (const auto& [module, entries] : importsByModule) {
		out << "#[link(wasm_import_module = " << quoteName(module, true) << ")]\n";
		out << "extern \"C\" {\n";
		for (const Json* entry : entries) {
			const std::string name = "wasmjit_"
					+ sanitize(module) + "_"
					+ sanitize(entry->at("name").get<std::string>());
			out << "    // G2H import: " << importComment(*entry) << "\n";
			out << "    #[link_name = "
				<< quoteName(entry->at("name").get<std::string>(), true) << "]\n";
			out << "    pub fn " << name << "(";
			emitRustParams(out, *entry,
					signatureResults(*entry).size() > 1u);
			out << ")";
			emitRustReturn(out, signatureResults(*entry));
			out << ";\n";
		}
		out << "}\n\n";
	}
	out << nonFunctionNotes(schema);
	return out.str();
}

} // namespace WASM::Bindings
