#ifndef WASMJIT_BINDINGS_FRONTEND_CPP_CPPFRONTEND_HPP
#define WASMJIT_BINDINGS_FRONTEND_CPP_CPPFRONTEND_HPP

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

/*!
  \file CppFrontend.hpp
  \brief The optional C++ candidate-signature front-end (`TB-3`, `TB-4a`).

  This is a build-time tool. It parses a C++ header with SWIG and proposes
  function signatures; a proposal is only a candidate until it matches an
  engine descriptor. The engine schema remains the truth, and nothing here is
  linked into `WasmBase/` or a backend.
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
