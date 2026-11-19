#include "BindingSchema.hpp"

#include "EmitInternal.hpp"
#include "EmitShared.hpp"

#include <string_view>

namespace WASM::Bindings {

const char* languageName(Language language) noexcept
{
	switch (language) {
	case Language::CHeader: return "c-header";
	case Language::Rust: return "rust";
	case Language::TypeScript: return "typescript";
	case Language::AssemblyScript: return "assemblyscript";
	case Language::Zig: return "zig";
	case Language::Kotlin: return "kotlin";
	case Language::MoonBit: return "moonbit";
	case Language::Go: return "go";
	case Language::Dart: return "dart";
	case Language::Java: return "java";
	case Language::CSharp: return "csharp";
	case Language::FSharp: return "fsharp";
	case Language::Python: return "python";
	case Language::Ruby: return "ruby";
	case Language::Lua: return "lua";
	case Language::Swift: return "swift";
	case Language::DLang: return "d";
	case Language::Nim: return "nim";
	case Language::Crystal: return "crystal";
	case Language::Ada: return "ada";
	case Language::Pascal: return "pascal";
	case Language::Haskell: return "haskell";
	case Language::CppAdapter: return "cpp-adapter";
	case Language::CppTypedefs: return "cpp-typedefs";
	case Language::CppSource: return "cpp-source";
	case Language::Wit: return "wit";
	case Language::Witx: return "witx";
	}
	return "c-header";
}

bool parseLanguage(std::string_view name, Language& language) noexcept
{
	for (const Language candidate : { Language::CHeader, Language::Rust,
			Language::TypeScript, Language::AssemblyScript, Language::Zig,
			Language::Kotlin, Language::MoonBit, Language::Go, Language::Dart,
			Language::Java, Language::CSharp, Language::FSharp,
			Language::Python, Language::Ruby, Language::Lua,
			Language::Swift, Language::DLang, Language::Nim,
			Language::Crystal, Language::Ada, Language::Pascal,
			Language::Haskell, Language::CppAdapter,
			Language::CppTypedefs, Language::CppSource, Language::Wit,
			Language::Witx }) {
		if (name == languageName(candidate)) {
			language = candidate;
			return true;
		}
	}
	return false;
}

std::string emit(const Schema& input, Language language)
{
	nlohmann::ordered_json schema = input.toJson();
	detail::dedupeJsonSchema(schema);
	switch (language) {
	case Language::CHeader: return emitCHeader(input);
	case Language::Rust: return emitRustModule(input);
	case Language::TypeScript: return detail::emitTypeScriptImpl(schema, input);
	case Language::AssemblyScript: return detail::emitAssemblyScriptImpl(schema, input);
	case Language::Zig: return detail::emitZigImpl(schema, input);
	case Language::Kotlin: return detail::emitKotlinImpl(schema, input);
	case Language::MoonBit: return detail::emitMoonBitImpl(schema, input);
	case Language::Go: return detail::emitGoImpl(schema, input);
	case Language::Dart: return detail::emitDartImpl(schema, input);
	case Language::Java: return detail::emitJavaImpl(schema, input);
	case Language::CSharp: return detail::emitCSharpImpl(schema, input);
	case Language::FSharp: return detail::emitFSharpImpl(schema, input);
	case Language::Python: return detail::emitPythonImpl(schema, input);
	case Language::Ruby: return detail::emitRubyImpl(schema, input);
	case Language::Lua: return detail::emitLuaImpl(schema, input);
	case Language::Swift: return detail::emitSwiftImpl(schema, input);
	case Language::DLang: return detail::emitDLangImpl(schema, input);
	case Language::Nim: return detail::emitNimImpl(schema);
	case Language::Crystal: return detail::emitCrystalImpl(schema);
	case Language::Ada: return detail::emitAdaImpl(schema);
	case Language::Pascal: return detail::emitPascalImpl(schema);
	case Language::Haskell: return detail::emitHaskellImpl(schema);
	case Language::CppAdapter: return detail::emitCppAdapterImpl(schema);
	case Language::CppTypedefs: return detail::emitCppTypedefsImpl(schema);
	case Language::CppSource: return detail::emitCppSourceImpl(schema);
	case Language::Wit: return detail::emitWitImpl(schema, input);
	case Language::Witx: return detail::emitWitxImpl(schema);
	}
	return {};
}

std::string emit(const nlohmann::ordered_json& input, Language language)
{
	return emit(Schema::fromJson(input), language);
}

} // namespace WASM::Bindings
