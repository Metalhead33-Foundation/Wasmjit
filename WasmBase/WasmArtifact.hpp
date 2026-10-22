#ifndef WASMARTIFACT_HPP
#define WASMARTIFACT_HPP
#include "WasmSection.hpp"
#include <cstdint>
#include <memory>
#include <optional>

namespace WASM {

class Module;
class Component;

// The two kinds of artifact that share the `\0asm` magic.
enum class ArtifactKind : uint8_t {
	CoreModule,
	Component
};

// Component Model preamble values (the core module version is not checked).
inline constexpr uint16_t kComponentVersion = 0x000D;
inline constexpr uint16_t kComponentLayer = 0x0001;

struct Preamble {
	uint32_t version = 0;
	uint16_t layer = 0;
};

// Common base for a parsed core module and a parsed Component Model component.
// `Artifact::parse` is the "parse this file" entry point: it reads the first
// eight bytes and dispatches on the layer field, so a caller does not need to
// know in advance which kind it holds.
struct Artifact {
	ArtifactKind kind;
	uint32_t version = 0;
	uint16_t layer = 0;

	explicit Artifact(ArtifactKind artifactKind) : kind(artifactKind) {}
	virtual ~Artifact() = default;

	Artifact(const Artifact&) = default;
	Artifact& operator=(const Artifact&) = default;

	bool isCoreModule() const { return kind == ArtifactKind::CoreModule; }
	bool isComponent() const { return kind == ArtifactKind::Component; }

	// Downcasts; null when the artifact is the other kind.
	Module* asCoreModule();
	const Module* asCoreModule() const;
	Component* asComponent();
	const Component* asComponent() const;

	// Reads magic (4) + version (2) + layer (2) and validates the magic.
	static Preamble readPreamble(Elv::Io::Device& file);

	// Reads the preamble and returns a Module or a Component.
	static std::unique_ptr<Artifact> parse(Elv::Io::Device& file);

protected:
	// Decodes the body after the preamble. `end` bounds a nested body inside
	// a component; `std::nullopt` means "read to the device's end".
	virtual void parseBody(Elv::Io::Device& file, std::optional<uint64_t> end) = 0;
};

}

#endif // WASMARTIFACT_HPP