#ifndef EUPHCONSTBUFFERDEVICE_H
#define EUPHCONSTBUFFERDEVICE_H
#include <span>
#include "ElvIoDevice.hpp"
namespace Euph {
namespace Io {

/**
 * @brief A read-only I/O device wrapper for a constant memory buffer.
 * * This class implements the @ref Elv::Io::Device interface to treat a static
 * or constant memory span as a seekable file-like stream. Since it wraps
 * `const std::byte`, write operations are not supported.
 */
class ConstBufferDevice : public Elv::Io::Device
{
private:
	/** @brief The underlying memory span being read. */
	std::span<const std::byte> buff;

	/** @brief The current read/seek position (offset) within the buffer. */
	uintptr_t ptr;

public:
	/**
	 * @brief Utility to cast a typed span into a constant byte span.
	 * * @tparam T The type of the source span.
	 * @param b The source span to be cast.
	 * @return A @ref std::span<const std::byte> representing the same memory range.
	 */
	template <typename T>
	static std::span<const std::byte> span_cast(const std::span<const T>& b) {
		return std::span<const std::byte>(reinterpret_cast<const std::byte*>(b.data()), b.size_bytes());
	}

	/**
	 * @brief Constructs a new ConstBufferDevice.
	 * @param buff The constant byte span to wrap.
	 */
	ConstBufferDevice(const std::span<const std::byte>& buff);

	/**
	 * @brief Reads data from the internal buffer into a destination.
	 * * @param buffer Pointer to the destination memory.
	 * @param size Size of each element to read.
	 * @param count Number of elements to read.
	 * @return The number of full elements successfully read.
	 */
	size_t read(void* buffer, size_t size, size_t count) override;

	/**
	 * @brief Attempt to write to the device.
	 * @note As this is a constant buffer device, this operation will fail or return 0.
	 * * @return Always returns 0 as the buffer is read-only.
	 */
	size_t write(const void* buffer, size_t size, size_t count) override;

	/**
	 * @brief Changes the current position within the buffer.
	 * * @param offset The number of bytes to move.
	 * @param whence The reference point for the offset (SET, CUR, or END).
	 * @return 0 on success, or a non-zero error code if the seek is out of bounds.
	 */
	int seek(long offset, Elv::Io::SeekOrigin whence) override;

	/**
	 * @brief Gets the current position of the pointer.
	 * @return The current offset from the beginning of the buffer.
	 */
	long tell() override;

	/**
	 * @brief Gets the total size of the wrapped buffer.
	 * @return The size of the buffer in bytes.
	 */
	size_t size() override;

	/**
	 * @brief Checks if the read pointer has reached the end of the buffer.
	 * @return true if the pointer is at or beyond the buffer size, false otherwise.
	 */
	bool eof() override;

	/**
	 * @brief Gets the access mode of this device.
	 * @return Always returns @ref Elv::Io::Mode::READ.
	 */
	Elv::Io::Mode getMode() const override;

	/**
	 * @brief Flushes any pending output.
	 * @note This is a no-op for memory buffers.
	 * @return true.
	 */
	bool flush() override;

	/**
	 * @brief Validates the state of the device.
	 * @return true if the buffer is not empty or is properly initialized.
	 */
	bool isValid() const override;
};

} // namespace Io
} // namespace Euph
#endif // EUPHCONSTBUFFERDEVICE_H
