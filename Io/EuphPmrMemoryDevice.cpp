#include "EuphPmrMemoryDevice.hpp"
namespace Euph {
namespace Io {

bool PmrMemoryDevice::hasFlag(Elv::Io::Mode flag) const {
	return (static_cast<uint8_t>(m_mode) & static_cast<uint8_t>(flag)) != 0;
}

PmrMemoryDevice::PmrMemoryDevice(Elv::Io::Mode mode, std::pmr::memory_resource* resource)
	: m_data(resource), m_position(0), m_mode(mode), m_valid(true) {}

PmrMemoryDevice::PmrMemoryDevice(Elv::Io::Mode mode, std::pmr::vector<std::byte>&& moved_data)
	: m_data(std::move(moved_data)), m_position(0), m_mode(mode), m_valid(true) {}

PmrMemoryDevice::PmrMemoryDevice(PmrMemoryDevice&&) noexcept = default;

PmrMemoryDevice& PmrMemoryDevice::operator=(PmrMemoryDevice&&) noexcept = default;

PmrMemoryDevice::~PmrMemoryDevice() = default;

size_t PmrMemoryDevice::read(void* buffer, size_t size, size_t count) {
	if (!m_valid || !hasFlag(Elv::Io::Mode::READ) || size == 0 || count == 0) {
		return 0;
	}

	const size_t bytesRequested = size * count;
	const size_t bytesAvailable = m_data.size() > m_position ? m_data.size() - m_position : 0;

	if (bytesAvailable == 0) {
		return 0; // EOF
	}

	const size_t bytesToRead = std::min(bytesRequested, bytesAvailable);

	// memcpy is the fastest block-read approach available here
	std::memcpy(buffer, m_data.data() + m_position, bytesToRead);
	m_position += bytesToRead;

	// Return number of complete elements read
	return bytesToRead / size;
}

size_t PmrMemoryDevice::write(const void* buffer, size_t size, size_t count) {
	if (!m_valid || size == 0 || count == 0) {
		return 0;
	}

	// Must have WRITE or APPEND permissions
	if (!hasFlag(Elv::Io::Mode::WRITE) && !hasFlag(Elv::Io::Mode::APPEND)) {
		return 0;
	}

	// If append mode, always seek to the end before writing
	if (hasFlag(Elv::Io::Mode::APPEND)) {
		m_position = m_data.size();
	}

	const size_t bytesToWrite = size * count;
	const size_t requiredSize = m_position + bytesToWrite;

	// Resize the vector if we are writing past the current end.
	// Resize + memcpy is dramatically faster than reserve + push_back loop
	if (requiredSize > m_data.size()) {
		m_data.resize(requiredSize);
		// Note: resizing zero-initializes new elements. If m_position > old_size,
		// this perfectly emulates POSIX sparse file 'holes' by filling gaps with \0.
	}

	std::memcpy(m_data.data() + m_position, buffer, bytesToWrite);
	m_position += bytesToWrite;

	return count;
}

int PmrMemoryDevice::seek(long offset, Elv::Io::SeekOrigin whence) {
	if (!m_valid) return -1;

	long newPos = 0;

	switch (whence) {
		case Elv::Io::SeekOrigin::SET:
			newPos = offset;
			break;
		case Elv::Io::SeekOrigin::CUR:
			newPos = static_cast<long>(m_position) + offset;
			break;
		case Elv::Io::SeekOrigin::END:
			newPos = static_cast<long>(m_data.size()) + offset;
			break;
		default:
			return -1; // Invalid whence
	}

	// Disallow seeking before the beginning of the file
	if (newPos < 0) {
		return -1;
	}

	// We allow seeking past the end (similar to POSIX lseek).
	// Writing after such a seek will expand the vector and zero-pad the gap.
	m_position = static_cast<size_t>(newPos);
	return 0;
}

long PmrMemoryDevice::tell() {
	return m_valid ? static_cast<long>(m_position) : -1;
}

size_t PmrMemoryDevice::size() {
	return m_valid ? m_data.size() : 0;
}

bool PmrMemoryDevice::eof() {
	return m_valid && (m_position >= m_data.size());
}

Elv::Io::Mode PmrMemoryDevice::getMode() const {
	return m_mode;
}

bool PmrMemoryDevice::flush() {
	// In-memory devices have no underlying OS buffers to sync.
	// Operations are inherently committed as soon as write() returns.
	return m_valid;
}

bool PmrMemoryDevice::isValid() const {
	return m_valid;
}

const std::pmr::vector<std::byte>& PmrMemoryDevice::getBuffer() const {
	return m_data;
}

std::pmr::vector<std::byte>& PmrMemoryDevice::getBuffer()
{
	return m_data;
}

void PmrMemoryDevice::yield(std::pmr::vector<std::byte>& target)
{
	target = std::move(m_data);
	m_valid = false;
}

}
}
