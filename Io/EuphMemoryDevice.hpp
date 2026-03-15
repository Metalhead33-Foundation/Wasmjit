#ifndef EUPHMEMORYDEVICE_HPP
#define EUPHMEMORYDEVICE_HPP
#include <vector>
#include <memory_resource>
#include <cstring>
#include <algorithm>
#include "ElvIoDevice.hpp"
namespace Euph {
namespace Io {

/**
 * @brief In-memory implementation of Device using a standard memory resource vector.
 */
class MemoryDevice : public Elv::Io::Device {
private:
	std::vector<std::byte> m_data;
	size_t m_position;
	Elv::Io::Mode m_mode;
	bool m_valid;

	// Helper to check bitwise enum flags
	bool hasFlag(Elv::Io::Mode flag) const;

public:
	/**
	 * @brief Constructs a new Memory Device.
	 * * @param mode Access mode for the device.
	 */
	MemoryDevice(Elv::Io::Mode mode);
	/**
	 * @brief Constructs a new Memory Device.
	 * * @param mode Access mode for the device.
	 * * @param moved_data Vector whose ownership is acquired.
	 */
	MemoryDevice(Elv::Io::Mode mode, std::vector<std::byte>&& moved_data);

	// Delete copy semantics as device handles are typically unique or shared via pointers
	MemoryDevice(const MemoryDevice&) = delete;
	MemoryDevice& operator=(const MemoryDevice&) = delete;

	// Allow move semantics
	MemoryDevice(MemoryDevice&&) noexcept;
	MemoryDevice& operator=(MemoryDevice&&) noexcept;

	~MemoryDevice() override;

	size_t read(void* buffer, size_t size, size_t count) override;

	size_t write(const void* buffer, size_t size, size_t count) override;

	int seek(long offset, Elv::Io::SeekOrigin whence) override;

	long tell() override;

	size_t size() override;

	bool eof() override;

	Elv::Io::Mode getMode() const override;

	bool flush() override;

	bool isValid() const override;

	// --- Specific to MemoryDevice ---

	/**
	 * @brief Direct read-only access to the underlying memory buffer.
	 */
	const std::vector<std::byte>& getBuffer() const;
	/**
	 * @brief Direct read-and-write access to the underlying memory buffer.
	 */
	std::vector<std::byte>& getBuffer();
	/**
	 * @brief Yields ownership of the underlying memory buffer. Warning - DO NOT USE DEVICE AFTERWARDS!
	 */
	void yield(std::vector<std::byte>& target);
};

} // namespace Io
} // namespace Euph
#endif // EUPHMEMORYDEVICE_HPP
