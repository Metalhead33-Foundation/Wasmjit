#include "WasmArtifact.hpp"
#include "WasmComponent.hpp"
#include "WasmModule.hpp"
#include <stdexcept>
#include <string>

namespace WASM {

Preamble Artifact::readPreamble(Elv::Io::Device& file)
{
	DWasmStream stream(file);
	uint32_t magic = 0;
	uint16_t versionLow = 0;
	uint16_t layer = 0;
	stream >> magic >> versionLow >> layer;
	if (magic != 0x6D736100)
		throw std::runtime_error("Invalid WASM module!\nMismatch in the magic number!");
	return Preamble{versionLow, layer};
}

std::unique_ptr<Artifact> Artifact::parse(Elv::Io::Device& file)
{
	const Preamble preamble = readPreamble(file);

	if (preamble.layer == 0) {
		auto module = std::make_unique<Module>();
		module->version = preamble.version;
		module->layer = 0;
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