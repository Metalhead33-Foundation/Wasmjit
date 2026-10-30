#include "WasmComponent.hpp"
#include "WasmSafety.hpp"
#include "WasmStore.hpp"
#include <Euphemy/Io/EuphConstBufferDevice.hpp>
#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace WASM {

namespace {
#include "WasmComponentParser.inc"
}

Component::Component()
	: Artifact(ArtifactKind::Component)
{
}

void Component::fromFile(Elv::Io::Device& file, Store& store)
{
	store_ = &store;
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
		Elv::Io::Device& file, uint64_t end, unsigned depth, Store& store)
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
	component->store_ = &store;
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
	validate();
}

void Component::parseSections(Elv::Io::Device& file, std::optional<uint64_t> end,
		unsigned depth)
{
	if (depth > kMaxComponentNesting)
		throw std::runtime_error("component nesting exceeds the limit (" +
				std::to_string(kMaxComponentNesting) + ")");

	const uint64_t limit = end ? *end : (file.size() != 0
			? static_cast<uint64_t>(file.size()) : 0);
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

		const uint64_t offset = file.tell() >= 0
				? static_cast<uint64_t>(file.tell()) : 0;
		if (limit != 0 && (offset > limit || size > limit - offset))
			throw std::runtime_error("component section size exceeds the input");

		std::vector<uint8_t> payload(size);
		if (size != 0)
			file.read(payload.data(), 1, size);

		ComponentSection section{type, size, offset, {}, {}};
		parseSectionPayload(section, std::move(payload), depth);
		sections.push_back(std::move(section));

		if (limit == 0)
			break;
	}
}

void Component::parseSectionPayload(ComponentSection& section,
		std::vector<uint8_t> payload, unsigned depth)
{
	static const std::byte emptyPayload{0};
	const std::byte* payloadData = payload.empty()
			? &emptyPayload
			: reinterpret_cast<const std::byte*>(payload.data());
	std::span<const std::byte> span(payloadData, payload.size());
	Euph::Io::ConstBufferDevice device(span);

	switch (section.type) {
	case ComponentSectionType::Custom: {
		Reader r(device, store().types().store());
		section.name = r.name();
		section.data = std::move(payload);
		break;
	}
	case ComponentSectionType::CoreModule:
		coreModules.push_back(Module::parseNested(device, payload.size(), store()));
		definitions.push_back({ComponentDefinitionKind::CoreModule,
				static_cast<uint32_t>(coreModules.size() - 1)});
		break;
	case ComponentSectionType::NestedComponent:
		components.push_back(Component::parseNested(device, payload.size(), depth + 1, store()));
		definitions.push_back({ComponentDefinitionKind::Component,
				static_cast<uint32_t>(components.size() - 1)});
		break;
	default: {
		Reader r(device, store().types().store());
		switch (section.type) {
		case ComponentSectionType::CoreInstance:
			r.vec([&] {
				coreInstances.push_back(parseCoreInstance(r));
				definitions.push_back({ComponentDefinitionKind::CoreInstance,
						static_cast<uint32_t>(coreInstances.size() - 1)});
			});
			break;
		case ComponentSectionType::CoreType:
			r.vec([&] {
				coreTypes.push_back(parseCoreTypeDef(r));
				definitions.push_back({ComponentDefinitionKind::CoreType,
						static_cast<uint32_t>(coreTypes.size() - 1)});
			});
			break;
		case ComponentSectionType::Instance:
			r.vec([&] {
				instances.push_back(parseComponentInstance(r));
				definitions.push_back({ComponentDefinitionKind::Instance,
						static_cast<uint32_t>(instances.size() - 1)});
			});
			break;
		case ComponentSectionType::Alias:
			r.vec([&] {
				aliases.push_back(parseAlias(r));
				definitions.push_back({ComponentDefinitionKind::Alias,
						static_cast<uint32_t>(aliases.size() - 1)});
			});
			break;
		case ComponentSectionType::Type:
			r.vec([&] {
				types.push_back(parseComponentTypeDef(r));
				definitions.push_back({ComponentDefinitionKind::Type,
						static_cast<uint32_t>(types.size() - 1)});
			});
			break;
		case ComponentSectionType::Canon:
			r.vec([&] {
				canons.push_back(parseCanon(r));
				definitions.push_back({ComponentDefinitionKind::Canon,
						static_cast<uint32_t>(canons.size() - 1)});
			});
			break;
		case ComponentSectionType::Start:
			starts.push_back(parseStart(r));
			definitions.push_back({ComponentDefinitionKind::Start,
					static_cast<uint32_t>(starts.size() - 1)});
			break;
		case ComponentSectionType::Import:
			r.vec([&] {
				imports.push_back(parseImportDecl(r));
				definitions.push_back({ComponentDefinitionKind::Import,
						static_cast<uint32_t>(imports.size() - 1)});
			});
			break;
		case ComponentSectionType::Export:
			r.vec([&] {
				exports.push_back(parseComponentExport(r));
				definitions.push_back({ComponentDefinitionKind::Export,
						static_cast<uint32_t>(exports.size() - 1)});
			});
			break;
		case ComponentSectionType::Value:
			r.vec([&] {
				values.push_back(parseValue(r));
				definitions.push_back({ComponentDefinitionKind::Value,
						static_cast<uint32_t>(values.size() - 1)});
			});
			break;
		default:
			throw std::runtime_error("component parse: unsupported section");
		}
		r.expectEnd();
		break;
	}
	}
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