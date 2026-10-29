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
class UnresolvedImportException : public std::exception {
private:
	//! The rendered message, built once in the constructor so `what()` cannot
	//! allocate or throw.
	std::string str;
	//! \param module the name of the module whose import could not be resolved
	//! \param import the field name of that import
	//! \return the message the exception reports
	static std::string getErrStr(std::string_view module, std::string_view import);
	//! exception interface
public:
	//! \param module the name of the module whose import could not be resolved
	//! \param import the field name of that import
	UnresolvedImportException(std::string_view module, std::string_view import);
	//! \return the message rendered when the exception was constructed
	const char* what() const noexcept override;
};
//! Thrown when a data or element segment does not fit the memory or table it is
//! initialised into. Publicly derived from `std::exception`, so an embedder
//! catches it with a normal `catch`.
class SegmentOutOfBoundsException : public std::exception {
private:
	//! The rendered message, built once in the constructor so `what()` cannot
	//! allocate or throw.
	std::string str;
	//! \param offset the offset the segment is written at
	//! \param size the number of bytes (or elements) the segment carries
	//! \param memorySize the size of the target memory or table
	//! \return the message the exception reports
	static std::string getErrStr(size_t offset, size_t size, size_t memorySize);
	//! exception interface
public:
	//! \param offset the offset the segment is written at
	//! \param size the number of bytes (or elements) the segment carries
	//! \param memorySize the size of the target memory or table
	SegmentOutOfBoundsException(size_t offset, size_t size, size_t memorySize);
	//! \return the message rendered when the exception was constructed
	const char* what() const noexcept override;
};
//! Thrown when a constant expression contains an opcode the decoder does not
//! know. `T` is the numeric type the offending opcode was read into, so the
//! message can print it in hexadecimal.
template <typename T> class InvalidOpcodeException : public std::exception {
private:
	//! The rendered message, built once in the constructor so `what()` cannot
	//! allocate or throw.
	std::string str;
	//! \param data the opcode that was read
	//! \return the message the exception reports, with the opcode in hex
	static std::string getErrStr(const T& data) {
		std::stringstream sstrm;
		sstrm << "Unsupported opcode in constant expression: 0x" << std::hex << data << std::endl;
		return sstrm.str();
	}
	//! exception interface
public:
	//! \param data the opcode that the constant expression used
	InvalidOpcodeException(const T& data) : str(getErrStr(data)) {}
	//! \return the rendered message of this exception
	const char* what() const noexcept override { return str.c_str(); }
};

}
WASMJIT_VISIBLE_END
#endif // WASMEXCEPTION_HPP
