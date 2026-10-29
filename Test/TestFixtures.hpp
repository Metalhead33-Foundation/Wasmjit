#ifndef TESTFIXTURES_HPP
#define TESTFIXTURES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Shared binary fixtures for the surface tests. Keeping them here means the
// runtime, plugin and ABI tests instantiate the same bytes.

namespace TestFixtures {

// (module
//   (func (export "answer") (result i32) i32.const 42))
inline constexpr std::array<std::uint8_t, 39> kAnswerModule = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
	0x03, 0x02, 0x01, 0x00,
	0x07, 0x0a, 0x01, 0x06, 0x61, 0x6e, 0x73, 0x77, 0x65, 0x72, 0x00, 0x00,
	0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b,
};

inline std::span<const std::byte> answerModuleBytes()
{
	return std::span<const std::byte>(
			reinterpret_cast<const std::byte*>(kAnswerModule.data()),
			kAnswerModule.size());
}

} // namespace TestFixtures

#endif // TESTFIXTURES_HPP
