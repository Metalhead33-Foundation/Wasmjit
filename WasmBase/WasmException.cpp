#include "WasmException.hpp"
namespace WASM {

std::string UnresolvedImportException::getErrStr(std::string_view module, std::string_view import)
{
	std::stringstream sstr;
	sstr << "Unresolved import: " << module << "." << import << std::endl;
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
/*
			throw InstantiationError(
				"Data segment out of bounds: offset=" +
				std::to_string(offset) + " size=" +
				std::to_string(seg.data.size()) +
				" memorySize=" + std::to_string(instance.ctx.memorySize));
*/
std::string SegmentOutOfBoundsException::getErrStr(size_t offset, size_t size, size_t memorySize)
{
	std::stringstream sstr;
	sstr << "Data segment out of bounds: offset=" << offset << " size=" << size << " memorySize=" << memorySize << std::endl;;
	return sstr.str();
}

SegmentOutOfBoundsException::SegmentOutOfBoundsException(size_t offset, size_t size, size_t memorySize)
	: str(getErrStr(offset,size,memorySize))
{
}

const char* SegmentOutOfBoundsException::what() const noexcept
{
	return str.c_str();
}

}