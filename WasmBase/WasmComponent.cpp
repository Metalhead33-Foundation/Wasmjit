#include "WasmComponent.hpp"
#include "WasmSafety.hpp"
#include <stdexcept>
#include <string>
#include <utility>

namespace WASM {

namespace {

uint64_t bodyLimit(Elv::Io::Device& file, std::optional<uint64_t> end)
{
	if (end)
		return *end;
	const std::size_t total = file.size();
	return total != 0 ? static_cast<uint64_t>(total) : 0;
}

}

Component::Component()
	: Artifact(ArtifactKind::Component)
{
}

void Component::fromFile(Elv::Io::Device& file)
{
	const Preamble preamble = Artifact::readPreamble(file);
	if (preamble.layer != kComponentLayer)
		throw std::runtime_error("Unsupported WebAssembly artifact: expected a "
				"Component Model component (layer 1).");
	if (preamble.version != kComponentVersion)
		throw std::runtime_error("Unsupported Component Model version " +
				std::to_string(preamble.version) + " (expected 0x000D).");
	version = preamble.version;
	layer = preamble.layer;
	parseBody(file, std::nullopt);
}

std::unique_ptr<Component> Component::parseNested(
		Elv::Io::Device& file, uint64_t end, unsigned depth)
{
	const long position = file.tell();
	if (position < 0 || static_cast<uint64_t>(position) > end
			|| 8 > end - static_cast<uint64_t>(position))
		throw std::runtime_error("nested component is smaller than its preamble");

	const Preamble preamble = Artifact::readPreamble(file);
	if (preamble.layer != kComponentLayer)
		throw std::runtime_error("nested component has a non-component layer");
	if (preamble.version != kComponentVersion)
		throw std::runtime_error("unsupported nested Component Model version " +
				std::to_string(preamble.version));

	auto component = std::make_unique<Component>();
	component->version = preamble.version;
	component->layer = preamble.layer;
	component->parseSections(file, end, depth);
	return component;
}

std::span<const ComponentSection> Component::getSections() const
{
	return sections;
}

void Component::parseBody(Elv::Io::Device& file, std::optional<uint64_t> end)
{
	parseSections(file, end, 0);
}

void Component::parseSections(Elv::Io::Device& file, std::optional<uint64_t> end,
		unsigned depth)
{
	if (depth > kMaxComponentNesting)
		throw std::runtime_error("component nesting exceeds the limit (" +
				std::to_string(kMaxComponentNesting) + ")");

	const uint64_t limit = bodyLimit(file, end);
	DWasmStream stream(file);

	while (true) {
		if (limit != 0 && file.tell() >= 0
				&& static_cast<uint64_t>(file.tell()) >= limit)
			break;
		if (sections.size() >= kMaxComponentSections)
			throw std::runtime_error("component has too many sections (limit " +
					std::to_string(kMaxComponentSections) + ")");

		uint8_t id = 0;
		uint32_t size = 0;
		stream >> id >> Elv::Io::Leb(size);
		if (id > static_cast<uint8_t>(ComponentSectionType::Value))
			throw std::runtime_error("malformed component section id " +
					std::to_string(id));
		const auto type = static_cast<ComponentSectionType>(id);

		const uint64_t payload = file.tell() >= 0
				? static_cast<uint64_t>(file.tell()) : 0;
		if (limit != 0 && (payload > limit || size > limit - payload))
			throw std::runtime_error("component section size exceeds the input");
		const uint64_t sectionEnd = payload + size;

		sections.push_back(ComponentSection{type, size, payload, {}});

		switch (type) {
		case ComponentSectionType::Custom:
			processCustomSection(file, sectionEnd);
			break;
		case ComponentSectionType::CoreModule:
			coreModules.push_back(Module::parseNested(file, sectionEnd));
			break;
		case ComponentSectionType::NestedComponent:
			components.push_back(Component::parseNested(file, sectionEnd, depth + 1));
			break;
		default:
			// Types, aliases, canon, imports/exports, etc. are recorded as
			// metadata only in this milestone.
			break;
		}

		file.seek(static_cast<long>(sectionEnd), Elv::Io::SeekOrigin::SET);
		if (limit == 0)
			break;
	}
}

void Component::processCustomSection(Elv::Io::Device& file, uint64_t sectionEnd)
{
	DWasmStream stream(file);
	uint32_t length = 0;
	stream >> Elv::Io::Leb(length);
	const uint64_t position = file.tell() >= 0
			? static_cast<uint64_t>(file.tell()) : 0;
	if (position > sectionEnd || length > sectionEnd - position)
		throw std::runtime_error("component custom section name out of bounds");
	std::string name(length, '\0');
	if (length != 0)
		file.read(name.data(), 1, length);
	sections.back().name = std::move(name);
}

const char* getComponentSectionTypeName(ComponentSectionType type)
{
	switch (type) {
	case ComponentSectionType::Custom: return "Custom";
	case ComponentSectionType::CoreModule: return "CoreModule";
	case ComponentSectionType::CoreInstance: return "CoreInstance";
	case ComponentSectionType::CoreType: return "CoreType";
	case ComponentSectionType::NestedComponent: return "NestedComponent";
	case ComponentSectionType::Instance: return "Instance";
	case ComponentSectionType::Alias: return "Alias";
	case ComponentSectionType::Type: return "Type";
	case ComponentSectionType::Canon: return "Canon";
	case ComponentSectionType::Start: return "Start";
	case ComponentSectionType::Import: return "Import";
	case ComponentSectionType::Export: return "Export";
	case ComponentSectionType::Value: return "Value";
	default: return "Unknown";
	}
}

}