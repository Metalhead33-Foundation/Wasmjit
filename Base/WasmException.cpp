#include "WasmException.hpp"
namespace WASM {

std::string UnresolvedImportException::getErrStr(std::string_view module, std::string_view import)
{
	std::stringstream sstr;
	sstr << "Unresolved import: " << module << "." << import;
	return sstr.str();
}

UnresolvedImportException::UnresolvedImportException(std::string_view module, std::string_view import)
	: str(getErrStr(module,import))
{
}

const char* UnresolvedImportException::what() const noexcept
{
	return str.c_str();
}

}