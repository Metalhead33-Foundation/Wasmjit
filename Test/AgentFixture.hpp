#ifndef AGENTFIXTURE_HPP
#define AGENTFIXTURE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

/*!
  \file AgentFixture.hpp
  \brief A shared-everything threads fixture for the multi-agent tests.

  It is authored as bytes because wasm-tools 1.252 cannot print the
  acquire-release ordering immediate or pause. The equivalent text is:

/**
 * \\brief Performs the module operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
    (module
/**
 * \\brief Performs the c operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
      (memory 1 1 shared)
/**
 * \\brief Performs the c operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
      (global $c (shared (mut i32)) (i32.const 0))
/**
 * \\brief Performs the func operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
      (func (export "produce") (param i32)       ;; [4]=arg; [0]=1 (release)
/**
 * \\brief Performs the store operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
        (i32.store (i32.const 4) (local.get 0))
/**
 * \\brief Performs the acqrel operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
        (i32.atomic.store acqrel (i32.const 0) (i32.const 1)))
/**
 * \\brief Performs the func operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
      (func (export "consume") (result i32)      ;; spin on [0] (acquire), then [4]
        (block $done (loop $spin
/**
 * \\brief Performs the done operation.
 * \\return The value or status produced by this operation.
 */
          (br_if $done (i32.atomic.load acqrel (i32.const 0)))
/**
 * \\brief Performs the load operation.
 * \\return The value or status produced by this operation.
 */
          (pause) (br $spin)))
/**
 * \\brief Performs the load operation.
 * \\return The value or status produced by this operation.
 */
        (i32.load (i32.const 4)))
/**
 * \\brief Performs the func operation.
 * \\return The value or status produced by this operation.
 */
      (func (export "wait_flag") (result i32)
/**
 * \\brief Performs the wait32 operation.
 * \\return The value or status produced by this operation.
 */
        (memory.atomic.wait32 (i32.const 8) (i32.const 0) (i64.const -1)))
/**
 * \\brief Performs the func operation.
 * \\return The value or status produced by this operation.
 */
      (func (export "notify_flag") (result i32)
/**
 * \\brief Performs the acqrel operation.
 * \\return The value or status produced by this operation.
 */
        (i32.atomic.store acqrel (i32.const 8) (i32.const 1))
/**
 * \\brief Performs the notify operation.
 * \\return The value or status produced by this operation.
 */
        (memory.atomic.notify (i32.const 8) (i32.const 1)))
/**
 * \\brief Performs the func operation.
 * \\return The value or status produced by this operation.
 */
      (func (export "reset")                     ;; [0]=0, [8]=0
        ...)
/**
 * \\brief Performs the func operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
      (func (export "boom") (unreachable))
/**
 * \\brief Performs the func operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
      (func (export "answer") (result i32) (i32.const 42))
/**
 * \\brief Performs the func operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
      (func (export "bump") (result i32)         ;; shared global RMW
/**
 * \\brief Performs the seqcst operation.
 * \\return The value or status produced by this operation.
 */
        (global.atomic.rmw.add seqcst (i32.const 1)))
/**
 * \\brief Performs the func operation.
 * \\return The value or status produced by this operation.
 */
      (func (export "counter") (result i32) (global.atomic.get seqcst))
/**
 * \\brief Performs the func operation.
 * \\return The value or status produced by this operation.
 */
      (func (export "mark_ready")                ;; [12]=1 (seqcst)
/**
 * \\brief Performs the seqcst operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
        (i32.atomic.store seqcst (i32.const 12) (i32.const 1)))
/**
 * \\brief Performs the func operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
      (func (export "ready") (result i32) (i32.atomic.load seqcst (i32.const 12)))
/**
 * \\brief Performs the func operation.
 * \\param i32 Input value or output destination for this operation.
 * \\return The value or status produced by this operation.
 */
      (func (export "notify_only") (result i32)    ;; no value change
        (memory.atomic.notify (i32.const 8) (i32.const 1))))

  This header is internal: it is not part of the embedding surface.
*/

namespace AgentFixture {

/*!
 * \brief Encoded shared-everything threads test module.
 *
 * The byte sequence defines the module described in the file-level
 * documentation, including atomic memory/global operations and wait/notify.
 */
inline constexpr std::array<std::uint8_t, 334> kModule = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0c, 0x03, 0x60,
	0x01, 0x7f, 0x00, 0x60, 0x00, 0x01, 0x7f, 0x60, 0x00, 0x00, 0x03, 0x0d,
	0x0c, 0x00, 0x01, 0x01, 0x01, 0x02, 0x02, 0x01, 0x01, 0x01, 0x02, 0x01,
	0x01, 0x05, 0x04, 0x01, 0x03, 0x01, 0x01, 0x06, 0x06, 0x01, 0x7f, 0x03,
	0x41, 0x00, 0x0b, 0x07, 0x7b, 0x0c, 0x07, 0x70, 0x72, 0x6f, 0x64, 0x75,
	0x63, 0x65, 0x00, 0x00, 0x07, 0x63, 0x6f, 0x6e, 0x73, 0x75, 0x6d, 0x65,
	0x00, 0x01, 0x09, 0x77, 0x61, 0x69, 0x74, 0x5f, 0x66, 0x6c, 0x61, 0x67,
	0x00, 0x02, 0x0b, 0x6e, 0x6f, 0x74, 0x69, 0x66, 0x79, 0x5f, 0x66, 0x6c,
	0x61, 0x67, 0x00, 0x03, 0x05, 0x72, 0x65, 0x73, 0x65, 0x74, 0x00, 0x04,
	0x04, 0x62, 0x6f, 0x6f, 0x6d, 0x00, 0x05, 0x06, 0x61, 0x6e, 0x73, 0x77,
	0x65, 0x72, 0x00, 0x06, 0x04, 0x62, 0x75, 0x6d, 0x70, 0x00, 0x07, 0x07,
	0x63, 0x6f, 0x75, 0x6e, 0x74, 0x65, 0x72, 0x00, 0x08, 0x0a, 0x6d, 0x61,
	0x72, 0x6b, 0x5f, 0x72, 0x65, 0x61, 0x64, 0x79, 0x00, 0x09, 0x05, 0x72,
	0x65, 0x61, 0x64, 0x79, 0x00, 0x0a, 0x0b, 0x6e, 0x6f, 0x74, 0x69, 0x66,
	0x79, 0x5f, 0x6f, 0x6e, 0x6c, 0x79, 0x00, 0x0b, 0x0a, 0x9b, 0x01, 0x0c,
	0x12, 0x00, 0x41, 0x04, 0x20, 0x00, 0x36, 0x02, 0x00, 0x41, 0x00, 0x41,
	0x01, 0xfe, 0x17, 0x12, 0x01, 0x00, 0x0b, 0x1a, 0x00, 0x02, 0x40, 0x03,
	0x40, 0x41, 0x00, 0xfe, 0x10, 0x12, 0x01, 0x00, 0x0d, 0x01, 0xfe, 0x04,
	0x0c, 0x00, 0x0b, 0x0b, 0x41, 0x04, 0x28, 0x02, 0x00, 0x0b, 0x0c, 0x00,
	0x41, 0x08, 0x41, 0x00, 0x42, 0x7f, 0xfe, 0x01, 0x02, 0x00, 0x0b, 0x13,
	0x00, 0x41, 0x08, 0x41, 0x01, 0xfe, 0x17, 0x12, 0x01, 0x00, 0x41, 0x08,
	0x41, 0x01, 0xfe, 0x00, 0x02, 0x00, 0x0b, 0x12, 0x00, 0x41, 0x00, 0x41,
	0x00, 0xfe, 0x17, 0x02, 0x00, 0x41, 0x08, 0x41, 0x00, 0xfe, 0x17, 0x02,
	0x00, 0x0b, 0x03, 0x00, 0x00, 0x0b, 0x04, 0x00, 0x41, 0x2a, 0x0b, 0x08,
	0x00, 0x41, 0x01, 0xfe, 0x51, 0x00, 0x00, 0x0b, 0x06, 0x00, 0xfe, 0x4f,
	0x00, 0x00, 0x0b, 0x0a, 0x00, 0x41, 0x0c, 0x41, 0x01, 0xfe, 0x17, 0x02,
	0x00, 0x0b, 0x08, 0x00, 0x41, 0x0c, 0xfe, 0x10, 0x02, 0x00, 0x0b, 0x0a,
	0x00, 0x41, 0x08, 0x41, 0x01, 0xfe, 0x00, 0x02, 0x00, 0x0b,
};

/*!
 * \brief Returns the encoded module as a byte span.
 * \return A non-owning span over kModule; its storage has static lifetime.
 */
/**
 * \\brief Performs the moduleBytes operation.
 * \\return The value or status produced by this operation.
 */
inline std::span<const std::byte> moduleBytes()
{
	return std::span<const std::byte>(
/**
 * \\brief Performs the data operation.
 * \\return The value or status produced by this operation.
 */
			reinterpret_cast<const std::byte*>(kModule.data()), kModule.size());
}

} // namespace AgentFixture

#endif // AGENTFIXTURE_HPP
