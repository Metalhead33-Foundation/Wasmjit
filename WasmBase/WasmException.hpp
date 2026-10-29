#ifndef WASMEXCEPTION_HPP
#define WASMEXCEPTION_HPP

#include "WasmApi.hpp"

/*!
  \file WasmException.hpp
  \brief Recoverable engine exceptions an embedder or the harness catches.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN
#include <exception>
#include <sstream>
namespace WASM {

//! These derive *publicly* from std::exception: an unresolved import or a
//! segment failure is an ordinary recoverable engine error, and embedders (and
//! the spec harness) catch it as `std::exception`. Private inheritance — the
//! `class` default — made every such error escape a normal catch block.
/** \brief Recoverable error raised when a module import cannot be resolved. */
class UnresolvedImportException : public std::exception {
private:
	std::string str;
/**
 * \brief Returns the requested backend/runtime property or view.
 * \param module Input value, resource descriptor, or output destination for this operation.
 * \param import Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
	static std::string getErrStr(std::string_view module, std::string_view import);
	//! exception interface
public:
	/** \brief Constructs an unresolved-import error. \param module Importing module name. \param import Missing import field name. */
/**
 * \brief Performs the UnresolvedImportException operation.
 * \param module Input value, resource descriptor, or output destination for this operation.
 * \param import Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
	UnresolvedImportException(std::string_view module, std::string_view import);
	/** \brief Returns the diagnostic message. \return Null-terminated message owned by this exception. */
/**
 * \brief Performs the what operation.
 * \return The requested value, handle, view, or success status.
 */
	const char* what() const noexcept override;
};
/** \brief Recoverable error raised when a data/element segment exceeds its target. */
class SegmentOutOfBoundsException : public std::exception {
private:
	std::string str;
/**
 * \brief Returns the requested backend/runtime property or view.
 * \param offset Input value, resource descriptor, or output destination for this operation.
 * \param size Input value, resource descriptor, or output destination for this operation.
 * \param memorySize Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
	static std::string getErrStr(size_t offset, size_t size, size_t memorySize);
	//! exception interface
public:
	/** \brief Constructs a segment-bounds error. \param offset Segment destination offset. \param size Segment byte length. \param memorySize Available target size in bytes. */
/**
 * \brief Performs the SegmentOutOfBoundsException operation.
 * \param offset Input value, resource descriptor, or output destination for this operation.
 * \param size Input value, resource descriptor, or output destination for this operation.
 * \param memorySize Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
	SegmentOutOfBoundsException(size_t offset, size_t size, size_t memorySize);
/**
 * \brief Performs the what operation.
 * \return The requested value, handle, view, or success status.
 */
	const char* what() const noexcept override;
};
/** \brief Recoverable error for an unsupported opcode in a constant expression. \tparam T Streamable opcode representation type. */
template <typename T> class InvalidOpcodeException : public std::exception {
private:
	std::string str;
/**
 * \brief Returns the requested backend/runtime property or view.
 * \param data Input value, resource descriptor, or output destination for this operation.
 * \return The requested value, handle, view, or success status.
 */
	static std::string getErrStr(const T& data) {
		std::stringstream sstrm;
		sstrm << "Unsupported opcode in constant expression: 0x" << std::hex << data << std::endl;
		return sstrm.str();
	}
	//! exception interface
public:
	/** \brief Constructs an invalid-opcode error. \param data Unsupported opcode value. */
/**
 * \brief Performs the InvalidOpcodeException operation.
 * \return The requested value, handle, view, or success status.
 */
	InvalidOpcodeException(const T& data) : str(getErrStr(data)) {}
	/** \brief Returns the diagnostic message. \return Null-terminated message owned by this exception. */
/**
 * \brief Performs the what operation.
 * \return The requested value, handle, view, or success status.
 */
	const char* what() const noexcept override { return str.c_str(); }
};

}
WASMJIT_VISIBLE_END
#endif // WASMEXCEPTION_HPP
