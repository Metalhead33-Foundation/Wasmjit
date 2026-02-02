#include "EuphFile.hpp"
#include <filesystem>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#elif defined (__unix)
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cerrno>
#else
#error "Unsupported operating system type!"
#endif
#include <cstring>
namespace Euph {
namespace Io {

File::File(const char* path, Elv::Io::Mode mode)
	: fileHandle(path, mode), mode(mode)
{
}

File::File(File&& mov)
	: fileHandle(std::move(mov.fileHandle)), mode(mov.mode)
{
}

File& File::operator=(File&& mov)
{
	this->fileHandle = std::move(mov.fileHandle);
	this->mode = mov.mode;
	return *this;
}

size_t File::read(void* buffer, size_t size, size_t count)
{
	return fileHandle.read(buffer,size,count);
}

size_t File::write(const void* buffer, size_t size, size_t count)
{
	return fileHandle.write(buffer,size,count);
}

int File::seek(long offset, Elv::Io::SeekOrigin whence)
{
	return fileHandle.seek(offset,whence);
}

long File::tell()
{
	return fileHandle.tell();
}

size_t File::size()
{
	return fileHandle.size();
}

bool File::eof()
{
	return fileHandle.eof();
}

Elv::Io::Mode File::getMode() const
{
	return mode;
}

bool File::flush()
{
	return fileHandle.flush();
}

bool File::isValid() const
{
#ifdef _WIN32
		return fileHandle.fileHandle != INVALID_HANDLE_VALUE;
#else
		return fileHandle.fileDescriptor != -1;
#endif
}

}
}

