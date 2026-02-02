#ifndef FILE_HPP
#define FILE_HPP
#include "ElvIoDevice.hpp"
#include "EuphPlatformDependentFileBase.hpp"

/**
 * @namespace Euph::Io
 * @brief Input/Output operations namespace for the Euph project.
 */
namespace Euph {
namespace Io {

/**
 * @class File
 * @brief Implementation of Elv::Io::Device for file operations.
 */
class File : public Elv::Io::Device
{
private:
	/**
	 * @var fileHandle
	 * @brief Handle to the file.
	 */
	PlatformDependentFileBase fileHandle;

	/**
	 * @var mode
	 * @brief Mode in which the file was opened.
	 */
	Elv::Io::Mode mode;

	/**
	 * @brief Copy constructor (deleted to prevent copying).
	 */
	File(const File& cpy) = delete;

	/**
	 * @brief Assignment operator (deleted to prevent copying).
	 */
	File& operator=(const File& cpy) = delete;

public:
	/**
	 * @brief Constructor for opening a file.
	 * @param path Path to the file.
	 * @param mode Mode in which to open the file (see Elv::Io::Mode).
	 */
	File(const char* path, Elv::Io::Mode mode);

	/**
	 * @brief Move constructor.
	 */
	File(File&& mov);

	/**
	 * @brief Move assignment operator.
	 */
	File& operator=(File&& mov);

	/**
	 * @copydoc Elv::Io::Device::read
	 */
	size_t read(void* buffer, size_t size, size_t count) override;

	/**
	 * @copydoc Elv::Io::Device::write
	 */
	size_t write(const void* buffer, size_t size, size_t count) override;

	/**
	 * @copydoc Elv::Io::Device::seek
	 */
	int seek(long offset, Elv::Io::SeekOrigin whence) override;

	/**
	 * @copydoc Elv::Io::Device::tell
	 */
	long tell() override;

	/**
	 * @copydoc Elv::Io::Device::size
	 */
	size_t size() override;

	/**
	 * @copydoc Elv::Io::Device::eof
	 */
	bool eof() override;

	/**
	 * @copydoc Elv::Io::Device::getMode
	 */
	Elv::Io::Mode getMode() const override;

	/**
	 * @copydoc Elv::Io::Device::flush
	 */
	bool flush() override;

	/**
	 * @copydoc Elv::Io::Device::isValid
	 */
	bool isValid() const override;
};

}
}

#endif // FILE_HPP
