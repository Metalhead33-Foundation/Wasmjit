#ifndef WASMEXCEPTION_HPP
#define WASMEXCEPTION_HPP
#include <exception>
#include <sstream>
namespace WASM {

class UnresolvedImportException : std::exception {
private:
	std::string str;
	static std::string getErrStr(std::string_view module, std::string_view import);
	// exception interface
public:
	UnresolvedImportException(std::string_view module, std::string_view import);
	const char* what() const noexcept override;
};
class SegmentOutOfBoundsException : std::exception {
private:
	std::string str;
	static std::string getErrStr(size_t offset, size_t size, size_t memorySize);
	// exception interface
public:
	SegmentOutOfBoundsException(size_t offset, size_t size, size_t memorySize);
	const char* what() const noexcept override;
};
template <typename T> class InvalidOpcodeException : std::exception {
private:
	std::string str;
	static std::string getErrStr(const T& data) {
		std::stringstream sstrm;
		sstrm << "Unsupported opcode in constant expression: 0x" << std::hex << data << std::endl;
		return sstrm.str();
	}
	// exception interface
public:
	InvalidOpcodeException(const T& data) : str(getErrStr(data)) {}
	const char* what() const noexcept override { return str.c_str(); }
};

}
#endif // WASMEXCEPTION_HPP
