#include "WasmArtifact.hpp"
#include "WasmComponent.hpp"
#include "WasmModule.hpp"
#include <stdexcept>
#include <string>

namespace WASM {

Preamble Artifact::readPreamble(Elv::Io::Device& file)
{
	// SAFETY §2: input-derived lengths are checked before they reach a buffer.
	// A truncated preamble used to index an empty span and abort the process.
	if (file.size() < 8)
		throw std::runtime_error(
				"Invalid WASM module! Truncated preamble.");
	DWasmStream stream(file);
	uint32_t magic = 0;
	uint16_t versionLow = 0;
	uint16_t layer = 0;
	stream >> magic >> versionLow >> layer;
	if (magic != 0x6D736100)
		throw std::runtime_error("Invalid WASM module!\nMismatch in the magic number!");
	// The core-module binary version is 1. Component preambles reuse the same
	// eight bytes with layer 1 and their own version; Artifact::parse checks
	// that version separately. A core module with any other version is a
	// malformed artifact, not a module this engine can decode
	// (docs/SCOPE.md §2: binary-encoded engine-owned input is a gap when it is
	// accepted without being understood).
	if (layer == 0 && versionLow != 1)
		throw std::runtime_error("unknown binary version " + std::to_string(versionLow));
	return Preamble{versionLow, layer};
}

std::unique_ptr<Artifact> Artifact::parse(Elv::Io::Device& file, Store& store)
{
	const Preamble preamble = readPreamble(file);

	if (preamble.layer == 0) {
		auto module = std::make_unique<Module>();
		module->version = preamble.version;
		module->layer = 0;
		module->store_ = &store;
		// Force the protected virtual to dispatch through the base type.
		Artifact* artifact = module.get();
		artifact->parseBody(file, std::nullopt);
		return module;
	}

	if (preamble.layer == kComponentLayer) {
		if (preamble.version != kComponentVersion)
			throw std::runtime_error("Unsupported Component Model version " +
					std::to_string(preamble.version) +
					" (expected 0x000D); see docs/proposals/COMPONENT_MODEL.md.");
		auto component = std::make_unique<Component>();
		component->version = preamble.version;
		component->layer = preamble.layer;
		component->store_ = &store;
		Artifact* artifact = component.get();
		artifact->parseBody(file, std::nullopt);
		return component;
	}

	throw std::runtime_error("Unsupported WebAssembly artifact layer " +
			std::to_string(preamble.layer) +
			"; expected 0 (core module) or 1 (component).");
}

Module* Artifact::asCoreModule()
{
	return isCoreModule() ? static_cast<Module*>(this) : nullptr;
}

const Module* Artifact::asCoreModule() const
{
	return isCoreModule() ? static_cast<const Module*>(this) : nullptr;
}

Component* Artifact::asComponent()
{
	return isComponent() ? static_cast<Component*>(this) : nullptr;
}

const Component* Artifact::asComponent() const
{
	return isComponent() ? static_cast<const Component*>(this) : nullptr;
}

}