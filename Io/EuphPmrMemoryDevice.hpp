#ifndef EUPHPMRMEMORYDEVICE_HPP
#define EUPHPMRMEMORYDEVICE_HPP
#include <vector>
#include <memory_resource>
#include <cstring>
#include <algorithm>
#include "ElvIoDevice.hpp"
namespace Euph {
namespace Io {

/**
 * @brief In-memory implementation of Device using a polymorphic memory resource vector.
 */
class PmrMemoryDevice : public Elv::Io::Device {
private:
	std::pmr::vector<std::byte> m_data;
	size_t m_position;
	Elv::Io::Mode m_mode;
	bool m_valid;

	// Helper to check bitwise enum flags
	bool hasFlag(Elv::Io::Mode flag) const;

public:
	/**
	 * @brief Constructs a new Memory Device.
	 * * @param mode Access mode for the device.
	 * @param resource Pointer to the polymorphic memory resource to use for allocations.
	 */
	PmrMemoryDevice(Elv::Io::Mode mode, std::pmr::memory_resource* resource = std::pmr::get_default_resource());
	/**
	 * @brief Constructs a new Memory Device.
	 * * @param mode Access mode for the device.
	 * * @param moved_data Vector whose ownership is acquired.
	 */
	PmrMemoryDevice(Elv::Io::Mode mode, std::pmr::vector<std::byte>&& moved_data);

	// Delete copy semantics as device handles are typically unique or shared via pointers
	PmrMemoryDevice(const PmrMemoryDevice&) = delete;
	PmrMemoryDevice& operator=(const PmrMemoryDevice&) = delete;

	// Allow move semantics
	PmrMemoryDevice(PmrMemoryDevice&&) noexcept;
	PmrMemoryDevice& operator=(PmrMemoryDevice&&) noexcept;

	~PmrMemoryDevice() override;

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
	const std::pmr::vector<std::byte>& getBuffer() const;
	/**
	 * @brief Direct read-and-write access to the underlying memory buffer.
	 */
	std::pmr::vector<std::byte>& getBuffer();
	/**
	 * @brief Yields ownership of the underlying memory buffer. Warning - DO NOT USE DEVICE AFTERWARDS!
	 */
	void yield(std::pmr::vector<std::byte>& target);
};

} // namespace Io
} // namespace Euph
#endif // EUPHPMRMEMORYDEVICE_HPP
