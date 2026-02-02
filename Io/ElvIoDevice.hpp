#ifndef ELVIODEVICE_HPP
#define ELVIODEVICE_HPP
#include <cstdint>
#include "ElvMathUtil.hpp"
#include "ElvAllocatorBasic.hpp"
#include <span>
#include <string>
#include <sstream>
#include <vector>

namespace Elv {
namespace Io {

/**
 * @brief Enumeration defining the origins for seeking within a file.
 *
 * These values are used to specify the reference point for file seek operations.
 */
enum class SeekOrigin : uint8_t {
	/**
	 * @brief Beginning of the file.
	 *
	 * Seeking from the start of the file. Offset is calculated from the first byte.
	 */
	SET,

	/**
	 * @brief Current position in the file.
	 *
	 * Seeking relative to the current file position. Offset can be negative, zero, or positive.
	 */
	CUR,

	/**
	 * @brief End of the file.
	 *
	 * Seeking from the end of the file. Typically used with a negative offset to move backwards from the end.
	 */
	END
};

/**
 * @brief Bitwise enumeration defining file access modes.
 *
 * These modes can be combined using bitwise OR operations to achieve multiple access types simultaneously.
 * For example, @ref READ_WRITE combines read and write permissions.
 */
enum class Mode : uint8_t {
	/**
	 * @brief Open the file for reading.
	 *
	 * Permission to read data from the file.
	 */
	READ = 1,

	/**
	 * @brief Open the file for writing.
	 *
	 * Permission to write data to the file. May truncate the file if it already exists.
	 */
	WRITE = 2,

	/**
	 * @brief Open the file for appending.
	 *
	 * Permission to write data to the end of the file. Preserves existing content.
	 */
	APPEND = 4,

	/**
	 * @brief Combination of @ref READ and @ref WRITE modes.
	 *
	 * Open the file for both reading and writing.
	 */
	READ_WRITE = (READ | WRITE),

	/**
	 * @brief Combination of @ref READ and @ref APPEND modes.
	 *
	 * Open the file for reading and appending. Less common, as appending typically doesn't require simultaneous read access.
	 */
	READ_APPEND = (READ | APPEND)
};

DEFINE_CLASS_WITH_POLYMORPHIC_ALLOCATOR(Device)
/**
 * @brief Abstract base class for file-like devices.
 *
 * The `Device` class defines a file interface with reading, writing,
 * seeking, and other common file operations. Derived classes should
 * implement these operations as needed for specific file types.
 */
class Device {
	public:
	/**
	 * @brief Reads data from the open file into a buffer.
	 *
	 * @param buffer   Pointer to the buffer to fill with data from the file.
	 * @param size	 Size of each element to read.
	 * @param count	Number of elements to read.
	 * @return		 Number of elements successfully read.
	 */
	virtual size_t read(void* buffer, size_t size, size_t count) = 0;

	/**
	 * @brief Writes data from a buffer to the open file.
	 *
	 * @param buffer   Pointer to the buffer containing data to write.
	 * @param size	 Size of each element to write.
	 * @param count	Number of elements to write.
	 * @return		 Number of elements successfully written.
	 */
	virtual size_t write(const void* buffer, size_t size, size_t count) = 0;

	/**
	 * @brief Seeks to a position in the open file.
	 *
	 * @param offset   Number of bytes to offset from the origin.
	 * @param whence   Origin from which to seek.
	 * @return		 0 upon success, non-zero on failure.
	 */
	virtual int seek(long offset, SeekOrigin whence) = 0;

	/**
	 * @brief Retrieves the current file position.
	 *
	 * @return Current position in the file in bytes from the beginning.
	 */
	virtual long tell() = 0;

	/**
	 * @brief Retrieves the complete file size.
	 *
	 * @return File size.
	 */
	virtual size_t size() = 0;

	/**
	 * @brief Checks if the end of the file has been reached.
	 *
	 * @return True if the end of the file has been reached, false otherwise.
	 */
	virtual bool eof() = 0;

	/**
	 * @brief Retrieves the current file access mode.
	 *
	 * Returns the mode in which the file was opened (e.g., read, write, append, or a combination).
	 *
	 * @return The current file access mode.
	 */
	virtual Mode getMode() const = 0;

	/**
	 * @brief Flushes the file buffer to ensure written data is committed.
	 *
	 * Forces any buffered write operations to be written to the underlying file, ensuring data integrity.
	 *
	 * @return True if the flush operation was successful, false otherwise.
	 */
	virtual bool flush() = 0;

	/**
	 * @brief Checks if the file device is in a valid state.
	 *
	 * Verifies the internal state of the file device, indicating whether it can be used for operations.
	 *
	 * @return True if the file device is valid, false otherwise.
	 */
	virtual bool isValid() const = 0;

	/**
	 * @brief Virtual destructor to ensure proper cleanup by derived classes.
	 */
	virtual ~Device() = default;

	// Convenience functions, wrappers

	/**
	 * @brief Reads data from the open file into a buffer.
	 *
	 * @tparam T	   The type of data to read.
	 * @param target   A span containing the buffer to fill with data from the file.
	 * @return		 Number of elements successfully read.
	 */
	template <typename T>
	inline size_t read(const std::span<T>& target) { return read(target.data(), sizeof(T), target.size()); }

	/**
	 * @brief Writes data from a buffer to the open file.
	 *
	 * @tparam T	   The type of data to write.
	 * @param source   A span containing the buffer with data to write to the file.
	 * @return		 Number of elements successfully written.
	 */
	template <typename T>
	inline size_t write(const std::span<const T>& source) { return write(source.data(), sizeof(T), source.size() ); }

	/**
	 * @brief Reads the entire file into a vector.
	 *
	 * @tparam T	   The type of data to read.
	 * @tparam Alloc   Allocator type used for the vector.
	 * @param dst	  The vector to fill with the file's content.
	 */
	template<class T, class Alloc = std::allocator<T>>
	requires Util::Allocator<Alloc, T>
	inline void readAllAsVector(std::vector<T, Alloc>& dst) {
		auto sz = Util::div_ceil(size_t(size() - tell()), sizeof(T));
		dst.resize(sz, 0);
		read(&dst[0], sz);
	}

	/**
	 * @brief Reads the entire file into a new vector.
	 *
	 * @tparam T	   The type of data to read.
	 * @tparam Alloc   Allocator type used for the vector.
	 * @return		 A vector containing the file's content.
	 */
	template<class T = std::byte, class Alloc = std::allocator<T>>
	requires Util::Allocator<Alloc, T>
	inline std::vector<T, Alloc> readAllAsVector() {
		std::vector<T, Alloc> tmp;
		readAllAsVector(tmp);
		return tmp;
	}

	/**
	 * @brief Reads the entire file into a string.
	 *
	 * @tparam CharT   Character type used for the string.
	 * @tparam Traits  Character traits type.
	 * @tparam Alloc   Allocator type used for the string.
	 * @param dst	  The string to fill with the file's content.
	 */
	template<class CharT, class Traits = std::char_traits<CharT>, class Alloc = std::allocator<CharT>>
	requires Util::Allocator<Alloc, CharT>
	inline void readAllAsString(std::basic_string<CharT, Traits, Alloc>& dst) {
		auto sz = Util::div_ceil(size_t(size() - tell()), sizeof(CharT));
		dst.resize(sz, 0);
		read(&dst[0], sizeof(CharT), sz);
		dst.shrink_to_fit();
	}

	/**
	 * @brief Reads the entire file into a new string.
	 *
	 * @tparam CharT   Character type used for the string.
	 * @tparam Traits  Character traits type.
	 * @tparam Alloc   Allocator type used for the string.
	 * @return		 A string containing the file's content.
	 */
	template<class CharT, class Traits = std::char_traits<CharT>, class Alloc = std::allocator<CharT>>
	requires Util::Allocator<Alloc, CharT>
	inline std::basic_string<CharT, Traits, Alloc> readAllAsString() {
		std::basic_string<CharT, Traits, Alloc> tmp;
		readAllAsString(tmp);
		return tmp;
	}

	/**
	 * @brief Reads a single line from the file into a stringstream.
	 *
	 * Reads characters one by one until a newline character is encountered.
	 *
	 * @tparam CharT   Character type used for the string.
	 * @tparam Traits  Character traits type.
	 * @tparam Alloc   Allocator type used for the stringstream.
	 * @param sstrm	The stringstream to fill with the line's content.
	 */
	template<class CharT, class Traits = std::char_traits<CharT>, class Alloc = std::allocator<CharT>>
	requires Util::Allocator<Alloc, CharT>
	inline void readLine(std::basic_stringstream<CharT, Traits, Alloc>& sstrm) {
		CharT tmp = 0;
		while (read(&tmp, sizeof(CharT), 1) && tmp != '\n') {
			sstrm << tmp;
		}
	}

	/**
	 * @brief Reads a single line from the file into a string.
	 *
	 * Reads characters one by one until a newline character is encountered.
	 *
	 * @tparam CharT   Character type used for the string.
	 * @tparam Traits  Character traits type.
	 * @tparam Alloc   Allocator type used for the string.
	 * @return		 A string containing the line's content.
	 */
	template<class CharT, class Traits = std::char_traits<CharT>, class Alloc = std::allocator<CharT>>
	requires Util::Allocator<Alloc, CharT>
	inline std::basic_string<CharT, Traits, Alloc> readLine() {
		std::basic_stringstream<CharT, Traits, Alloc> sstrm;
		readLine(sstrm);
		return sstrm.str();
	}
};

}
}
#endif // ELVIODEVICE_HPP
