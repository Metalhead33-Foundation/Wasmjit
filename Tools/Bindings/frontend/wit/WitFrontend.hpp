#ifndef WASMJIT_BINDINGS_FRONTEND_WIT_WITFRONTEND_HPP
#define WASMJIT_BINDINGS_FRONTEND_WIT_WITFRONTEND_HPP

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "BindingModel.hpp"

/*!
  \file WitFrontend.hpp
  \brief WIT/WIT-JSON to the neutral binding schema.

  WIT is a build-time interface description. This front-end parses the JSON
  emitted by `wasm-tools component wit --json` and, for text WIT, asks the
  configured `wasm-tools` executable to produce that JSON first. It is tooling
  only: no WIT parser or package resolver is part of `WasmBase/`.
*/
namespace WASM::Bindings {

/*!
 * \brief One typed WIT type-table entry.
 */
struct WitTypeInfo {
	std::size_t id = 0; //!< Type-table id.
	std::string name; //!< Declared WIT name, empty when anonymous.
	std::string displayName; //!< Generated target-language name.
	nlohmann::ordered_json kind; //!< Raw WIT kind (record, variant, ...).
	int ownerInterface = -1; //!< Owning interface id, or -1.
	int ownerPackage = -1; //!< Owning package id, or -1.
	bool alias = false; //!< True for an alias entry.
};

/*!
 * \brief One typed WIT function in an interface or world.
 */
struct WitFunctionInfo {
	std::string name; //!< WIT function name.
	nlohmann::ordered_json raw; //!< The complete WIT function object.
	bool async = false; //!< True for an async WIT function kind.
};

/*!
 * \brief One typed WIT interface.
 */
struct WitInterfaceInfo {
	std::size_t id = 0; //!< Interface id.
	std::string name; //!< Declared interface name.
	int package = -1; //!< Owning package id, or -1.
	std::vector<WitFunctionInfo> functions; //!< Interface functions.
	std::vector<std::size_t> types; //!< Type ids exposed by the interface.
};

/*!
 * \brief One typed WIT world.
 */
struct WitWorldInfo {
	std::size_t id = 0; //!< World id.
	std::string name; //!< Declared world name.
	int package = -1; //!< Owning package id, or -1.
	nlohmann::ordered_json imports; //!< Raw imports object.
	nlohmann::ordered_json exports; //!< Raw exports object.
};

/*!
 * \brief One typed WIT package.
 */
struct WitPackageInfo {
	std::size_t id = 0; //!< Package id.
	std::string name; //!< Qualified package name.
	std::vector<std::size_t> interfaces; //!< Package interface ids.
	std::vector<std::size_t> worlds; //!< Package world ids.
};

/*!
 * \brief The typed package/interface/world/type graph of one WIT document.
 */
struct WitModel {
	std::vector<WitTypeInfo> types; //!< Type table.
	std::vector<WitInterfaceInfo> interfaces; //!< Interfaces.
	std::vector<WitWorldInfo> worlds; //!< Worlds.
	std::vector<WitPackageInfo> packages; //!< Packages.
};

/*!
 * \brief Parses a WIT JSON document into the typed package graph.
 *
 * \param document The `wasm-tools component wit --json` document.
 * \param model Receives the parsed graph.
 * \param error Receives a diagnostic when the document is malformed.
 * \return true on success.
 */
bool parseWitModel(const nlohmann::ordered_json& document, WitModel& model,
		std::string& error);

/*!
 * \brief How the front-end treats an async WIT function.
 */
enum class WitAsyncPolicy {
	Reject, //!< Reject async WIT instead of generating a synchronous stub.
	AllowSynchronous //!< Emit a documented synchronous stub.
};

/*!
 * \brief Resolves one WIT type reference through a WIT JSON type table.
 *
 * Scalar aliases resolve to their underlying scalar; records, variants,
 * lists, resources and other structured types resolve to a named opaque WIT
 * reference.
 *
 * \param document A document containing a `types` array.
 * \param type A WIT type id, scalar spelling or type object.
 * \return The neutral schema value type.
 */
nlohmann::ordered_json witResolveType(const nlohmann::ordered_json& document,
		const nlohmann::ordered_json& type);

/*!
 * \brief Returns a deterministic target-language name for one WIT type id.
 *
 * A named WIT type keeps its WIT spelling. An anonymous structured type gets a
 * semantic name derived from its kind and children, with the type-table id
 * appended so that duplicate structural types remain distinct identifiers.
 *
 * \param document A document containing a `types` array.
 * \param id The WIT type-table id.
 * \return The unsanitized display name.
 */
std::string witTypeDisplayName(const nlohmann::ordered_json& document,
		std::size_t id);

/*!
 * \brief Selects and converts one WIT world from a WIT JSON document.
 *
 * The first world is used when \p worldName is empty. A world function becomes
 * a function entry; an interface import/export contributes its functions under
 * the interface name. The result is a complete schema, then projected to
 * \p direction exactly like a descriptor-derived schema.
 *
 * \param document The `wasm-tools component wit --json` document.
 * \param direction The requested output direction.
 * \param moduleName The module/component name in the schema envelope.
 * \param worldName The world to select, or empty for the first world.
 * \param error Receives a diagnostic when the document or world is invalid.
 * \param asyncPolicy How async WIT functions are handled.
 * \return The projected typed schema, or an empty schema on error.
 */
Schema witSchema(const nlohmann::ordered_json& document, Direction direction,
		std::string_view moduleName, std::string_view worldName,
		std::string& error,
		WitAsyncPolicy asyncPolicy = WitAsyncPolicy::AllowSynchronous,
		std::string_view packageName = {});

/*!
 * \brief Loads a WIT JSON file or a WIT text/package path.
 *
 * A `.json` path is parsed directly. Any other path is passed to
 * `wasm-tools component wit --json`; the `WASM_TOOLS` environment variable can
 * override the executable name. The text path may be a `.wit` file or a
 * WIT package directory.
 *
 * \param path WIT JSON, WIT text, or WIT package directory.
 * \param direction The requested output direction.
 * \param moduleName The module/component name in the schema envelope.
 * \param worldName The world to select, or empty for the first world.
 * \param forceJson Treat \p path as WIT JSON even without a `.json` extension.
 * \param error Receives a diagnostic when the file/tool/JSON is invalid.
 * \param asyncPolicy How async WIT functions are handled.
 * \param packageName The package to select, or empty for any package.
 * \return The projected typed schema, or an empty schema on error.
 */
Schema witSchemaFromFile(const std::filesystem::path& path, Direction direction,
		std::string_view moduleName, std::string_view worldName, bool forceJson,
		std::string& error,
		WitAsyncPolicy asyncPolicy = WitAsyncPolicy::AllowSynchronous,
		std::string_view packageName = {});

/*!
 * \brief Parses a WITX file into the neutral core-module schema.
 *
 * WITX is a Preview 1 / legacy interface description. The core module
 * descriptor remains authoritative for the binary ABI; this front-end preserves
 * WITX module, function, parameter and result names for semantic binding output.
 *
 * \param path The `.witx` file.
 * \param direction The requested output direction.
 * \param moduleName The module name in the schema envelope, or empty for the
 *        WITX module name.
 * \param error Receives a diagnostic when the file or WITX syntax is invalid.
 * \return The projected schema, or an empty schema on error.
 */
Schema witxSchemaFromFile(const std::filesystem::path& path, Direction direction,
		std::string_view moduleName, std::string& error);

} // namespace WASM::Bindings

#endif // WASMJIT_BINDINGS_FRONTEND_WIT_WITFRONTEND_HPP
