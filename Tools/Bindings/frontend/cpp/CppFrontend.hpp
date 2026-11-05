#ifndef WASMJIT_BINDINGS_FRONTEND_CPP_CPPFRONTEND_HPP
#define WASMJIT_BINDINGS_FRONTEND_CPP_CPPFRONTEND_HPP

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "BindingModel.hpp"

/*!
  \file CppFrontend.hpp
  \brief The optional C++ candidate-signature front-end (`TB-3`, `TB-4a`).

  This is a build-time tool. It parses a C++ header with SWIG (or a Glue
  registration source) and proposes function signatures. A proposal is a
  candidate until it matches an engine descriptor; a candidate-derived schema
  can also drive an emitter, but it is labelled as such and never modifies a
  descriptor-built schema. Nothing here is linked into `WasmBase/` or a
  backend.
*/
namespace WASM::Bindings {

/*! \brief One proposed parameter, in the header's C++ spelling. */
struct CandidateParameter {
	std::string type; //!< The parsed C++ type spelling.
	std::string name; //!< The parsed parameter name.
};

/*! \brief One proposed function signature from a C++ header. */
struct CandidateSignature {
	std::string name; //!< The parsed function name.
	std::string returnType; //!< The parsed C++ return type, or `void`.
	std::vector<CandidateParameter> params; //!< The parsed parameters.
	std::string description; //!< The comment immediately above the declaration.
};

/*!
 * \brief Parses a C++ header through SWIG's XML front-end.
 *
 * SWIG is invoked as a build-time tool; its output is parsed into candidate
 * signatures only. It does not validate against a module, and it never links
 * into the engine.
 *
 * \param header The C++ header to parse.
 * \param error Receives a diagnostic when SWIG is unavailable or fails.
 * \return The proposed signatures; empty when `error` is non-empty.
 */
std::vector<CandidateSignature> swigCandidates(const std::filesystem::path& header,
		std::string& error);

/*!
 * \brief Reads a Glue registration source through Glue's declaration printer.
 *
 * The source must define `glue::MapValue wasmjit_bindings()`. A generated
 * driver links it against the installed Glue library, prints declarations and
 * parses the function entries into candidates. This is a build-time path only:
 * Glue 1.5 exposes runtime maps, not a C++ header parser.
 *
 * \param source The C++ source that defines `wasmjit_bindings()`.
 * \param error Receives a diagnostic when the source cannot be compiled or run.
 * \return The proposed signatures; empty when `error` is non-empty.
 */
std::vector<CandidateSignature> glueCandidates(const std::filesystem::path& source,
		std::string& error);

/*!
 * \brief Builds a typed schema for host functions that are exposed to Wasm.
 *
 * Only candidates whose first parameter is a `VMContext*`/`VMContext&` are
 * included; that first parameter is part of the `Callable` ABI, not a Wasm
 * parameter, so it is removed from the schema. Every remaining parameter and
 * the result must map to a core Wasm type; a candidate that cannot be mapped is
 * skipped. Parameter names and leading comments are carried into `argNames`
 * and `description`, so every target language uses them.
 *
 * The candidates are host implementations, so they are **imports** of the Wasm
 * module for `g2h` and `all`. Asking for `h2g` or `g2g` is an error: a
 * `VMContext*` C++ function is not a guest export.
 *
 * \param candidates The signatures proposed by a C++ front-end.
 * \param direction The requested direction; `g2h` or `all`.
 * \param moduleName The Wasm import module recorded in the envelope.
 * \param frontend The front-end name recorded in the schema.
 * \return A typed schema whose imports are the fitting functions.
 * \throws std::runtime_error when \p direction cannot describe host imports.
 */
Schema candidateSchemaModel(
		const std::vector<CandidateSignature>& candidates,
		Direction direction, std::string_view moduleName,
		std::string_view frontend = "candidate");

/*!
 * \brief Builds a JSON schema for host functions that are exposed to Wasm.
 * \param candidates The signatures proposed by a C++ front-end.
 * \param direction The requested direction spelling (`"g2h"` or `"all"`).
 * \param moduleName The Wasm import module recorded in the envelope.
 * \param frontend The front-end name recorded in the schema.
 * \return The ordered JSON form of `candidateSchemaModel()`.
 */
nlohmann::ordered_json candidateSchema(
		const std::vector<CandidateSignature>& candidates,
		std::string_view direction, std::string_view moduleName,
		std::string_view frontend = "candidate");

/*!
 * \brief Validates candidate signatures against a binding schema.
 *
 * A candidate matches only when its name, arity, result shape and scalar types
 * agree with a function entry in the schema. The report is informational: the
 * schema is not modified and no candidate enters it.
 *
 * \param schema The schema produced by `moduleSchema()` or `componentSchema()`.
 * \param candidates The signatures proposed by a C++ front-end.
 * \param frontend The front-end name recorded in the report.
 * \return An ordered JSON report with per-candidate match verdicts.
 */
nlohmann::ordered_json crossCheckCandidates(const nlohmann::ordered_json& schema,
		const std::vector<CandidateSignature>& candidates,
		std::string_view frontend = "swig");

} // namespace WASM::Bindings

#endif // WASMJIT_BINDINGS_FRONTEND_CPP_CPPFRONTEND_HPP
