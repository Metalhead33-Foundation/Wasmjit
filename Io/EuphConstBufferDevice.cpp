#include "EuphConstBufferDevice.hpp"
#include <cstring>
namespace Euph {
namespace Io {

ConstBufferDevice::ConstBufferDevice(const std::span<const std::byte>& buff)
	: buff(buff), ptr(0)
{

}
size_t ConstBufferDevice::read(void* buffer, size_t size, size_t count)
{
	const size_t toCpy = std::min(size*count,buff.size() - ptr);
	memcpy(buffer,buff.data(),toCpy);
	return toCpy / size;
}

size_t ConstBufferDevice::write(const void* buffer, size_t size, size_t count)
{
	return 0;
}

int ConstBufferDevice::seek(long offset, Elv::Io::SeekOrigin whence)
{
	switch (whence) {
		case Elv::Io::SeekOrigin::SET: ptr = offset; break;
		case Elv::Io::SeekOrigin::CUR: ptr += offset; break;
		case Elv::Io::SeekOrigin::END: ptr = static_cast<uintptr_t>(buff.size() - offset); break;
			break;
	}
	return 0;
}

long ConstBufferDevice::tell()
{
	return ptr;
}

size_t ConstBufferDevice::size()
{
	return buff.size();
}

bool ConstBufferDevice::eof()
{
	return ptr == buff.size();
}

Elv::Io::Mode ConstBufferDevice::getMode() const
{
	return Elv::Io::Mode::READ;
}

bool ConstBufferDevice::flush()
{
	return true;
}

bool ConstBufferDevice::isValid() const
{
	return true;
}

}
}