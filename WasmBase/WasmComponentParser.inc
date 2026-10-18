// ── Bounded reader over one section payload ────────────────────────────────
class Reader {
public:
	Reader(Elv::Io::Device& source, TypeStore& typeStore)
		: types(typeStore), device(source), stream(source) {}

	TypeStore& types;

	DWasmStream& raw() { return stream; }

	uint64_t end() const { return static_cast<uint64_t>(device.size()); }
	uint64_t position() const
	{
		const long p = device.tell();
		if (p < 0)
			throw std::runtime_error("component parse: negative input position");
		return static_cast<uint64_t>(p);
	}
	uint64_t remaining() const
	{
		const uint64_t e = end();
		const uint64_t p = position();
		if (p > e)
			throw std::runtime_error("component parse: read past the section end");
		return e - p;
	}
	void require(uint64_t count, const char* what) const
	{
		if (count > remaining())
			throw std::runtime_error(std::string("component parse: ") + what +
					" claims more bytes than the section has left");
	}

	uint8_t u8()
	{
		require(1, "a byte");
		uint8_t value = 0;
		stream >> value;
		return value;
	}
	uint32_t u32()
	{
		require(1, "a u32");
		uint32_t value = 0;
		stream >> Elv::Io::Leb(value);
		return value;
	}
	int32_t sleb32()
	{
		require(1, "an s32");
		int32_t value = 0;
		stream >> Elv::Io::Leb(value);
		return value;
	}
	std::string name()
	{
		const uint32_t length = u32();
		require(length, "a name");
		std::string value(length, '\0');
		if (length != 0)
			device.read(value.data(), 1, length);
		return value;
	}
	std::vector<uint8_t> bytes(uint32_t length, const char* what)
	{
		require(length, what);
		std::vector<uint8_t> value(length);
		if (length != 0)
			device.read(value.data(), 1, length);
		return value;
	}
	uint32_t count(const char* what)
	{
		const uint32_t n = u32();
		// Every component vector element occupies at least one byte.
		require(n, what);
		return n;
	}
	template <typename F> void vec(F&& element)
	{
		const uint32_t n = count("a component vector");
		for (uint32_t i = 0; i < n; ++i)
			element();
	}
	void expectEnd() const
	{
		if (position() != end())
			throw std::runtime_error("component parse: trailing bytes in section");
	}

	Elv::Io::Device& device;

private:
	DWasmStream stream;
};

PrimitiveValueType primitiveFromCode(uint8_t raw)
{
	switch (raw) {
	case 0x7F: return PrimitiveValueType::Bool;
	case 0x7E: return PrimitiveValueType::S8;
	case 0x7D: return PrimitiveValueType::U8;
	case 0x7C: return PrimitiveValueType::S16;
	case 0x7B: return PrimitiveValueType::U16;
	case 0x7A: return PrimitiveValueType::S32;
	case 0x79: return PrimitiveValueType::U32;
	case 0x78: return PrimitiveValueType::S64;
	case 0x77: return PrimitiveValueType::U64;
	case 0x76: return PrimitiveValueType::F32;
	case 0x75: return PrimitiveValueType::F64;
	case 0x74: return PrimitiveValueType::Char;
	case 0x73: return PrimitiveValueType::String;
	case 0x64: return PrimitiveValueType::ErrorContext;
	default: return PrimitiveValueType::Invalid;
	}
}

ComponentValueType parseValueType(Reader& r)
{
	const int32_t code = r.sleb32();
	if (code >= 0) {
		ComponentValueType type;
		type.kind = ComponentValueType::Kind::TypeIndex;
		type.typeIndex = static_cast<uint32_t>(code);
		return type;
	}
	const uint8_t raw = static_cast<uint8_t>(code & 0x7F);
	const PrimitiveValueType primitive = primitiveFromCode(raw);
	if (primitive == PrimitiveValueType::Invalid)
		throw std::runtime_error("component parse: invalid value-type opcode 0x" +
				std::to_string(raw));
	ComponentValueType type;
	type.kind = ComponentValueType::Kind::Primitive;
	type.primitive = primitive;
	return type;
}

std::optional<ComponentValueType> parseOptionalValueType(Reader& r)
{
	const uint8_t tag = r.u8();
	if (tag == 0x00)
		return std::nullopt;
	if (tag != 0x01)
		throw std::runtime_error("component parse: invalid optional value type");
	return parseValueType(r);
}

std::optional<bool> parseAsyncFlag(Reader& r)
{
	const uint8_t tag = r.u8();
	if (tag == 0x00)
		return std::nullopt;
	if (tag == 0x01)
		return true;
	throw std::runtime_error("component parse: invalid async flag");
}

std::optional<bool> parseSharedFlag(Reader& r)
{
	const uint8_t tag = r.u8();
	if (tag == 0x00)
		return std::nullopt;
	if (tag == 0x01)
		return true;
	throw std::runtime_error("component parse: invalid shared flag");
}

CoreSort coreSortFromByte(uint8_t raw)
{
	switch (raw) {
	case 0x00: return CoreSort::Func;
	case 0x01: return CoreSort::Table;
	case 0x02: return CoreSort::Memory;
	case 0x03: return CoreSort::Global;
	case 0x04: return CoreSort::Tag;
	case 0x10: return CoreSort::Type;
	case 0x11: return CoreSort::Module;
	case 0x12: return CoreSort::Instance;
	default:
		throw std::runtime_error("component parse: invalid core sort " +
				std::to_string(raw));
	}
}

ComponentSort componentSortFromByte(uint8_t raw)
{
	switch (raw) {
	case 0x00: return ComponentSort::Core;
	case 0x01: return ComponentSort::Func;
	case 0x02: return ComponentSort::Value;
	case 0x03: return ComponentSort::Type;
	case 0x04: return ComponentSort::Component;
	case 0x05: return ComponentSort::Instance;
	default:
		throw std::runtime_error("component parse: invalid component sort " +
				std::to_string(raw));
	}
}

// A sort without an index (used by alias definitions).
SortIdx parseSort(Reader& r)
{
	const uint8_t tag = r.u8();
	SortIdx sort;
	if (tag == 0x00) {
		sort.sort = ComponentSort::Core;
		sort.core = CoreSortIdx{coreSortFromByte(r.u8()), 0};
	} else {
		sort.sort = componentSortFromByte(tag);
	}
	return sort;
}

CoreSortIdx parseCoreSortIdx(Reader& r)
{
	CoreSortIdx sort;
	sort.sort = coreSortFromByte(r.u8());
	sort.index = r.u32();
	return sort;
}

SortIdx parseSortIdx(Reader& r)
{
	const uint8_t tag = r.u8();
	SortIdx sort;
	if (tag == 0x00) {
		sort.sort = ComponentSort::Core;
		sort.core = CoreSortIdx{coreSortFromByte(r.u8()), r.u32()};
	} else {
		sort.sort = componentSortFromByte(tag);
		sort.index = r.u32();
	}
	return sort;
}
// ── Core type definitions (core:type / core:moduletype) ───────────────────
void parseCoreComposite(Reader& r, uint8_t first, Subtype& subtype)
{
	if (first == 0x60) {
		FuncType type;
		const uint32_t params = r.count("core function parameters");
		for (uint32_t i = 0; i < params; ++i) {
			StorageType value;
			value.decode(r.raw());
			type.params.push_back(value);
		}
		const uint32_t results = r.count("core function results");
		for (uint32_t i = 0; i < results; ++i) {
			StorageType value;
			value.decode(r.raw());
			type.results.push_back(value);
		}
		subtype.composite = type;
	} else if (first == 0x5F) {
		StructType type;
		const uint32_t fields = r.count("core struct fields");
		for (uint32_t i = 0; i < fields; ++i) {
			FieldType field;
			field.storageType.decode(r.raw());
			const uint8_t mut = r.u8();
			if (mut > 1)
				throw std::runtime_error("component parse: malformed field mutability");
			field.isMutable = (mut == 1);
			type.fields.push_back(field);
		}
		subtype.composite = type;
	} else if (first == 0x5E) {
		ArrayType type;
		type.elementType.storageType.decode(r.raw());
		const uint8_t mut = r.u8();
		if (mut > 1)
			throw std::runtime_error("component parse: malformed array mutability");
		type.elementType.isMutable = (mut == 1);
		subtype.composite = type;
	} else {
		throw std::runtime_error("component parse: invalid core composite type 0x" +
				std::to_string(first));
	}
}

Subtype parseCoreSubtype(Reader& r, uint8_t first)
{
	Subtype subtype;
	if (first == 0x50 || first == 0x4F) {
		subtype.isFinal = (first == 0x4F);
		const uint32_t parents = r.count("core supertypes");
		for (uint32_t i = 0; i < parents; ++i)
			subtype.supertypeIndices.push_back(r.u32());
		const uint8_t composite = r.u8();
		parseCoreComposite(r, composite, subtype);
	} else {
		subtype.isFinal = true;
		parseCoreComposite(r, first, subtype);
	}
	return subtype;
}

CoreRecType parseCoreRecType(Reader& r, uint8_t first)
{
	CoreRecType type;
	if (first == 0x4E) {
		type.isRecGroup = true;
		const uint32_t count = r.count("core recursive types");
		for (uint32_t i = 0; i < count; ++i) {
			const uint8_t member = r.u8();
			type.subtypes.push_back(parseCoreSubtype(r, member));
		}
	} else {
		type.subtypes.push_back(parseCoreSubtype(r, first));
	}
	return type;
}

CoreExternType parseCoreExternType(Reader& r)
{
	CoreExternType type;
	const uint8_t kind = r.u8();
	switch (kind) {
	case 0x00:
		type.kind = CoreExternType::Kind::Function;
		type.typeIndex = r.u32();
		break;
	case 0x01:
		type.kind = CoreExternType::Kind::Table;
		type.table.elementType.decode(r.raw());
		type.table.limits.decode(r.raw());
		break;
	case 0x02:
		type.kind = CoreExternType::Kind::Memory;
		type.memory.decode(r.raw());
		break;
	case 0x03:
		type.kind = CoreExternType::Kind::Global;
		type.global.contentType.decode(r.raw());
		{
			const uint8_t mut = r.u8();
			if (mut > 1)
				throw std::runtime_error("component parse: malformed global mutability");
			type.global.isMutable = (mut == 1);
		}
		break;
	case 0x04:
		type.kind = CoreExternType::Kind::Tag;
		type.tagAttribute = r.u8();
		type.typeIndex = r.u32();
		break;
	default:
		throw std::runtime_error("component parse: invalid core extern type " +
				std::to_string(kind));
	}
	return type;
}

CoreImport parseCoreImport(Reader& r)
{
	CoreImport import;
	import.module = r.name();
	import.name = r.name();
	import.type = parseCoreExternType(r);
	return import;
}

CoreExportDecl parseCoreExportDecl(Reader& r)
{
	CoreExportDecl exportDecl;
	exportDecl.name = r.name();
	exportDecl.type = parseCoreExternType(r);
	return exportDecl;
}

CoreOuterAlias parseCoreOuterAlias(Reader& r)
{
	CoreOuterAlias alias;
	if (r.u8() != 0x10 || r.u8() != 0x01)
		throw std::runtime_error("component parse: malformed core outer alias");
	alias.count = r.u32();
	alias.index = r.u32();
	return alias;
}

CoreModuleDecl parseCoreModuleDecl(Reader& r)
{
	CoreModuleDecl decl;
	const uint8_t kind = r.u8();
	switch (kind) {
	case 0x00:
		decl.kind = CoreModuleDecl::Kind::Import;
		decl.import = parseCoreImport(r);
		break;
	case 0x01: {
		decl.kind = CoreModuleDecl::Kind::Type;
		const uint8_t first = r.u8();
		decl.type = parseCoreRecType(r, first);
		break;
	}
	case 0x02:
		decl.kind = CoreModuleDecl::Kind::Alias;
		decl.alias = parseCoreOuterAlias(r);
		break;
	case 0x03:
		decl.kind = CoreModuleDecl::Kind::Export;
		decl.exportDecl = parseCoreExportDecl(r);
		break;
	default:
		throw std::runtime_error("component parse: invalid core module declaration " +
				std::to_string(kind));
	}
	return decl;
}

CoreTypeDef parseCoreTypeDef(Reader& r)
{
	CoreTypeDef type;
	const uint8_t first = r.u8();
	if (first == 0x50) {
		type.kind = CoreTypeDef::Kind::ModuleType;
		const uint32_t count = r.count("core module declarations");
		for (uint32_t i = 0; i < count; ++i)
			type.moduleDecls.push_back(parseCoreModuleDecl(r));
	} else if (first == 0x00) {
		type.kind = CoreTypeDef::Kind::RecType;
		if (r.u8() != 0x50)
			throw std::runtime_error("component parse: expected a core subtype");
		type.recType.subtypes.push_back(parseCoreSubtype(r, 0x50));
	} else {
		type.kind = CoreTypeDef::Kind::RecType;
		type.recType = parseCoreRecType(r, first);
	}
	return type;
}
// ── Component names, types and declarators ────────────────────────────────
ComponentTypeDef parseComponentTypeDef(Reader& r);
ComponentDecl parseComponentDecl(Reader& r);
InstanceDecl parseInstanceDecl(Reader& r);
ImportDecl parseImportDecl(Reader& r);
ExportDecl parseExportDecl(Reader& r);
NameAttributes parseNameAttributes(Reader& r);
ExternType parseExternType(Reader& r);

ComponentExport parseComponentExport(Reader& r)
{
	ComponentExport exportDecl;
	exportDecl.name = parseNameAttributes(r);
	exportDecl.sortIndex = parseSortIdx(r);
	const uint8_t tag = r.u8();
	if (tag == 0x01)
		exportDecl.type = parseExternType(r);
	else if (tag != 0x00)
		throw std::runtime_error("component parse: invalid export type tag");
	return exportDecl;
}
ExternType parseExternType(Reader& r);
Alias parseAlias(Reader& r);

Attribute parseAttribute(Reader& r)
{
	Attribute attribute;
	const uint8_t kind = r.u8();
	switch (kind) {
	case 0x00:
		attribute.kind = Attribute::Kind::Implements;
		attribute.text = r.name();
		break;
	case 0x01:
		attribute.kind = Attribute::Kind::VersionSuffix;
		attribute.text = r.name();
		break;
	case 0x02:
		attribute.kind = Attribute::Kind::ExternalId;
		attribute.text = r.name();
		break;
	default:
		throw std::runtime_error("component parse: invalid import/export attribute " +
				std::to_string(kind));
	}
	return attribute;
}

NameAttributes parseNameAttributes(Reader& r)
{
	NameAttributes attributes;
	attributes.tag = r.u8();
	if (attributes.tag > 0x02)
		throw std::runtime_error("component parse: invalid name attributes tag " +
				std::to_string(attributes.tag));
	attributes.name = r.name();
	if (attributes.tag == 0x02) {
		const uint32_t count = r.count("import/export attributes");
		for (uint32_t i = 0; i < count; ++i)
			attributes.attributes.push_back(parseAttribute(r));
	}
	return attributes;
}

TypeId parseDefinedValueType(Reader& r, uint8_t opcode)
{
	const PrimitiveValueType primitive = primitiveFromCode(opcode);
	if (primitive != PrimitiveValueType::Invalid)
		return r.types.addComponentPrimitive(primitive);
	switch (opcode) {
	case 0x72: { // record
		std::vector<ComponentField> fields;
		r.vec([&] {
			ComponentField field;
			field.name = r.types.internString(r.name());
			field.type = parseValueType(r);
			fields.push_back(field);
		});
		return r.types.addComponentRecord(fields);
	}
	case 0x71: { // variant
		std::vector<ComponentCase> cases;
		r.vec([&] {
			ComponentCase item;
			item.label = r.types.internString(r.name());
			const std::optional<ComponentValueType> type = parseOptionalValueType(r);
			item.hasType = type.has_value();
			if (type)
				item.type = *type;
			if (r.u8() != 0x00)
				throw std::runtime_error("component parse: malformed variant case");
			cases.push_back(item);
		});
		return r.types.addComponentVariant(cases);
	}
	case 0x70: // list
		return r.types.addComponentList(parseValueType(r));
	case 0x67: { // fixed-length list
		const ComponentValueType element = parseValueType(r);
		return r.types.addComponentFixedList(element, r.u32());
	}
	case 0x6F: { // tuple
		std::vector<ComponentValueType> elements;
		r.vec([&] { elements.push_back(parseValueType(r)); });
		return r.types.addComponentTuple(elements);
	}
	case 0x6E: { // flags
		std::vector<StringId> labels;
		r.vec([&] { labels.push_back(r.types.internString(r.name())); });
		return r.types.addComponentLabels(TypeKind::ComponentFlags, labels);
	}
	case 0x6D: { // enum
		std::vector<StringId> labels;
		r.vec([&] { labels.push_back(r.types.internString(r.name())); });
		return r.types.addComponentLabels(TypeKind::ComponentEnum, labels);
	}
	case 0x6B: // option
		return r.types.addComponentOption(parseValueType(r));
	case 0x6A: { // result
		const std::optional<ComponentValueType> ok = parseOptionalValueType(r);
		const std::optional<ComponentValueType> error = parseOptionalValueType(r);
		return r.types.addComponentResult(ok.value_or(ComponentValueType{}),
				error.value_or(ComponentValueType{}), ok.has_value(), error.has_value());
	}
	case 0x69: // own
		return r.types.addComponentTypeRef(TypeKind::ComponentOwn, r.u32());
	case 0x68: // borrow
		return r.types.addComponentTypeRef(TypeKind::ComponentBorrow, r.u32());
	case 0x66: { // stream
		const std::optional<ComponentValueType> element = parseOptionalValueType(r);
		return r.types.addComponentStream(TypeKind::ComponentStream,
				element.value_or(ComponentValueType{}), element.has_value());
	}
	case 0x65: { // future
		const std::optional<ComponentValueType> element = parseOptionalValueType(r);
		return r.types.addComponentStream(TypeKind::ComponentFuture,
				element.value_or(ComponentValueType{}), element.has_value());
	}
	case 0x63: { // map
		const ComponentValueType key = parseValueType(r);
		const ComponentValueType value = parseValueType(r);
		return r.types.addComponentMap(key, value);
	}
	default:
		throw std::runtime_error("component parse: invalid defined value type 0x" +
				std::to_string(opcode));
	}
}

TypeId parseComponentFuncType(Reader& r, bool isAsync)
{
	std::vector<ComponentFuncParam> params;
	const uint32_t paramCount = r.count("component function parameters");
	for (uint32_t i = 0; i < paramCount; ++i) {
		ComponentFuncParam param;
		param.name = r.types.internString(r.name());
		param.type = parseValueType(r);
		params.push_back(param);
	}
	std::vector<ComponentValueType> results;
	const uint8_t resultTag = r.u8();
	if (resultTag == 0x00) {
		results.push_back(parseValueType(r));
	} else if (resultTag == 0x01) {
		if (r.u8() != 0x00)
			throw std::runtime_error("component parse: malformed result list");
	} else {
		throw std::runtime_error("component parse: invalid result list tag");
	}
	return r.types.addComponentFunc(params, results, isAsync);
}

ComponentTypeDef parseComponentTypeDef(Reader& r)
{
	ComponentTypeDef type;
	const int32_t op = r.sleb32();
	const uint8_t opcode = static_cast<uint8_t>(op & 0x7F);

	if (opcode == 0x40 || opcode == 0x43) {
		type.kind = ComponentTypeDef::Kind::Func;
		type.id = parseComponentFuncType(r, opcode == 0x43);
		return type;
	}
	if (opcode == 0x41) {
		type.kind = ComponentTypeDef::Kind::Component;
		std::vector<ComponentDecl> declarations;
		const uint32_t count = r.count("component type declarations");
		for (uint32_t i = 0; i < count; ++i)
			declarations.push_back(parseComponentDecl(r));
		type.declarations = std::move(declarations);
		return type;
	}
	if (opcode == 0x42) {
		type.kind = ComponentTypeDef::Kind::Instance;
		std::vector<InstanceDecl> declarations;
		const uint32_t count = r.count("instance type declarations");
		for (uint32_t i = 0; i < count; ++i)
			declarations.push_back(parseInstanceDecl(r));
		type.declarations = std::move(declarations);
		return type;
	}
	if (opcode == 0x3F) {
		type.kind = ComponentTypeDef::Kind::Resource;
		ResourceType resource;
		resource.rep.decode(r.raw());
		const uint8_t hasDestructor = r.u8();
		if (hasDestructor == 0x01)
			resource.destructor = r.u32();
		else if (hasDestructor != 0x00)
			throw std::runtime_error("component parse: malformed resource destructor");
		ComponentResourcePayload payload;
		payload.rep = resource.rep;
		payload.hasDestructor = resource.destructor.has_value();
		payload.destructor = resource.destructor.value_or(0);
		type.id = r.types.addComponentResource(payload);
		type.declarations = resource;
		return type;
	}

	type.kind = ComponentTypeDef::Kind::DefinedValue;
	type.id = parseDefinedValueType(r, opcode);
	return type;
}

ComponentDecl parseComponentDecl(Reader& r)
{
	ComponentDecl decl;
	const uint8_t kind = r.u8();
	switch (kind) {
	case 0x03:
		decl.kind = ComponentDecl::Kind::Import;
		decl.payload = std::make_unique<ImportDecl>(parseImportDecl(r));
		break;
	case 0x00:
		decl.kind = ComponentDecl::Kind::CoreType;
		decl.payload = std::make_unique<CoreTypeDef>(parseCoreTypeDef(r));
		break;
	case 0x01:
		decl.kind = ComponentDecl::Kind::Type;
		decl.payload = std::make_unique<ComponentTypeDef>(parseComponentTypeDef(r));
		break;
	case 0x02:
		decl.kind = ComponentDecl::Kind::Alias;
		decl.payload = std::make_unique<Alias>(parseAlias(r));
		break;
	case 0x04:
		decl.kind = ComponentDecl::Kind::Export;
		decl.payload = std::make_unique<ExportDecl>(parseExportDecl(r));
		break;
	default:
		throw std::runtime_error("component parse: invalid component declaration " +
				std::to_string(kind));
	}
	return decl;
}

InstanceDecl parseInstanceDecl(Reader& r)
{
	InstanceDecl decl;
	const uint8_t kind = r.u8();
	switch (kind) {
	case 0x00:
		decl.kind = InstanceDecl::Kind::CoreType;
		decl.payload = std::make_unique<CoreTypeDef>(parseCoreTypeDef(r));
		break;
	case 0x01:
		decl.kind = InstanceDecl::Kind::Type;
		decl.payload = std::make_unique<ComponentTypeDef>(parseComponentTypeDef(r));
		break;
	case 0x02:
		decl.kind = InstanceDecl::Kind::Alias;
		decl.payload = std::make_unique<Alias>(parseAlias(r));
		break;
	case 0x04:
		decl.kind = InstanceDecl::Kind::Export;
		decl.payload = std::make_unique<ExportDecl>(parseExportDecl(r));
		break;
	default:
		throw std::runtime_error("component parse: invalid instance declaration " +
				std::to_string(kind));
	}
	return decl;
}
// ── Extern types, imports/exports and aliases ─────────────────────────────
TypeBound parseTypeBound(Reader& r)
{
	TypeBound bound;
	const uint8_t kind = r.u8();
	if (kind == 0x00) {
		bound.kind = TypeBound::Kind::Eq;
		bound.typeIndex = r.u32();
	} else if (kind == 0x01) {
		bound.kind = TypeBound::Kind::SubResource;
	} else {
		throw std::runtime_error("component parse: invalid type bound");
	}
	return bound;
}

ValueBound parseValueBound(Reader& r)
{
	ValueBound bound;
	const uint8_t kind = r.u8();
	if (kind == 0x00) {
		bound.kind = ValueBound::Kind::Eq;
		bound.valueIndex = r.u32();
	} else if (kind == 0x01) {
		bound.kind = ValueBound::Kind::Type;
		bound.type = parseValueType(r);
	} else {
		throw std::runtime_error("component parse: invalid value bound");
	}
	return bound;
}

ExternType parseExternType(Reader& r)
{
	ExternType type;
	const uint8_t kind = r.u8();
	switch (kind) {
	case 0x00:
		if (r.u8() != 0x11)
			throw std::runtime_error("component parse: malformed core-module extern type");
		type.kind = ExternType::Kind::CoreModule;
		type.typeIndex = r.u32();
		break;
	case 0x01:
		type.kind = ExternType::Kind::Func;
		type.typeIndex = r.u32();
		break;
	case 0x02:
		type.kind = ExternType::Kind::Value;
		type.value = parseValueBound(r);
		break;
	case 0x03:
		type.kind = ExternType::Kind::Type;
		type.typeBound = parseTypeBound(r);
		break;
	case 0x04:
		type.kind = ExternType::Kind::Component;
		type.typeIndex = r.u32();
		break;
	case 0x05:
		type.kind = ExternType::Kind::Instance;
		type.typeIndex = r.u32();
		break;
	default:
		throw std::runtime_error("component parse: invalid extern type " +
				std::to_string(kind));
	}
	return type;
}

ImportDecl parseImportDecl(Reader& r)
{
	ImportDecl import;
	import.name = parseNameAttributes(r);
	import.type = parseExternType(r);
	return import;
}

ExportDecl parseExportDecl(Reader& r)
{
	ExportDecl exportDecl;
	exportDecl.name = parseNameAttributes(r);
	exportDecl.type = parseExternType(r);
	return exportDecl;
}

Alias parseAlias(Reader& r)
{
	Alias alias;
	alias.sort = parseSort(r);
	const uint8_t kind = r.u8();
	switch (kind) {
	case 0x00:
		alias.kind = Alias::Kind::Export;
		alias.instanceIndex = r.u32();
		alias.name = r.name();
		break;
	case 0x01:
		alias.kind = Alias::Kind::CoreExport;
		alias.instanceIndex = r.u32();
		alias.name = r.name();
		break;
	case 0x02:
		alias.kind = Alias::Kind::Outer;
		alias.outerCount = r.u32();
		alias.index = r.u32();
		break;
	default:
		throw std::runtime_error("component parse: invalid alias kind " +
				std::to_string(kind));
	}
	return alias;
}
// ── Core/component instances, start and values ────────────────────────────
CoreInstance parseCoreInstance(Reader& r)
{
	CoreInstance instance;
	const uint8_t kind = r.u8();
	if (kind == 0x00) {
		instance.kind = CoreInstance::Kind::Instantiate;
		instance.moduleIndex = r.u32();
		r.vec([&] {
			CoreInstantiateArg arg;
			arg.name = r.name();
			if (r.u8() != 0x12)
				throw std::runtime_error("component parse: malformed core instantiate arg");
			arg.instanceIndex = r.u32();
			instance.args.push_back(std::move(arg));
		});
	} else if (kind == 0x01) {
		instance.kind = CoreInstance::Kind::InlineExports;
		r.vec([&] {
			CoreInlineExport exportDecl;
			exportDecl.name = r.name();
			exportDecl.sortIndex = parseCoreSortIdx(r);
			instance.exports.push_back(std::move(exportDecl));
		});
	} else {
		throw std::runtime_error("component parse: invalid core instance expression");
	}
	return instance;
}

ComponentInstance parseComponentInstance(Reader& r)
{
	ComponentInstance instance;
	const uint8_t kind = r.u8();
	if (kind == 0x00) {
		instance.kind = ComponentInstance::Kind::Instantiate;
		instance.componentIndex = r.u32();
		r.vec([&] {
			InstantiateArg arg;
			arg.name = r.name();
			arg.sortIndex = parseSortIdx(r);
			instance.args.push_back(std::move(arg));
		});
	} else if (kind == 0x01) {
		instance.kind = ComponentInstance::Kind::InlineExports;
		r.vec([&] {
			InlineExport exportDecl;
			exportDecl.name = parseNameAttributes(r);
			exportDecl.sortIndex = parseSortIdx(r);
			instance.exports.push_back(std::move(exportDecl));
		});
	} else {
		throw std::runtime_error("component parse: invalid instance expression");
	}
	return instance;
}

Start parseStart(Reader& r)
{
	Start start;
	start.funcIndex = r.u32();
	r.vec([&] { start.args.push_back(r.u32()); });
	start.resultCount = r.u32();
	return start;
}

ValueDecl parseValue(Reader& r)
{
	ValueDecl value;
	value.type = parseValueType(r);
	value.encodedLength = r.u32();
	value.encoded = r.bytes(value.encodedLength, "a component value");
	return value;
}
// ── Canonical definitions ──────────────────────────────────────────────────
std::vector<CanonOpt> parseCanonOpts(Reader& r)
{
	std::vector<CanonOpt> opts;
	r.vec([&] {
		CanonOpt opt;
		const uint8_t kind = r.u8();
		switch (kind) {
		case 0x00: opt.kind = CanonOptKind::Utf8; break;
		case 0x01: opt.kind = CanonOptKind::Utf16; break;
		case 0x02: opt.kind = CanonOptKind::Latin1Utf16; break;
		case 0x03: opt.kind = CanonOptKind::Memory; opt.index = r.u32(); break;
		case 0x04: opt.kind = CanonOptKind::Realloc; opt.index = r.u32(); break;
		case 0x05: opt.kind = CanonOptKind::PostReturn; opt.index = r.u32(); break;
		case 0x06: opt.kind = CanonOptKind::Async; break;
		case 0x07: opt.kind = CanonOptKind::Callback; opt.index = r.u32(); break;
		default:
			throw std::runtime_error("component parse: invalid canonopt " +
					std::to_string(kind));
		}
		opts.push_back(opt);
	});
	return opts;
}

std::vector<ComponentValueType> parseResultList(Reader& r)
{
	std::vector<ComponentValueType> results;
	const uint8_t tag = r.u8();
	if (tag == 0x00) {
		results.push_back(parseValueType(r));
	} else if (tag == 0x01) {
		if (r.u8() != 0x00)
			throw std::runtime_error("component parse: malformed result list");
	} else {
		throw std::runtime_error("component parse: invalid result list");
	}
	return results;
}

Canon parseCanon(Reader& r)
{
	Canon canon;
	const uint8_t op = r.u8();
	const auto expectZero = [&] {
		if (r.u8() != 0x00)
			throw std::runtime_error("component parse: malformed canon immediate");
	};

	switch (op) {
	case 0x00:
		expectZero();
		canon.kind = Canon::Kind::Lift;
		canon.coreFuncIndex = r.u32();
		canon.opts = parseCanonOpts(r);
		canon.typeIndex = r.u32();
		break;
	case 0x01:
		expectZero();
		canon.kind = Canon::Kind::Lower;
		canon.funcIndex = r.u32();
		canon.opts = parseCanonOpts(r);
		break;
	case 0x02:
		canon.kind = Canon::Kind::ResourceNew;
		canon.typeIndex = r.u32();
		break;
	case 0x03:
		canon.kind = Canon::Kind::ResourceDrop;
		canon.typeIndex = r.u32();
		break;
	case 0x04:
		canon.kind = Canon::Kind::ResourceRep;
		canon.typeIndex = r.u32();
		break;
	case 0x24:
		canon.kind = Canon::Kind::BackpressureInc;
		break;
	case 0x25:
		canon.kind = Canon::Kind::BackpressureDec;
		break;
	case 0x09:
		canon.kind = Canon::Kind::TaskReturn;
		canon.results = parseResultList(r);
		canon.opts = parseCanonOpts(r);
		break;
	case 0x05:
		canon.kind = Canon::Kind::TaskCancel;
		break;
	case 0x0A:
		canon.kind = Canon::Kind::ContextGet;
		canon.contextType.decode(r.raw());
		canon.contextIndex = r.u32();
		break;
	case 0x0B:
		canon.kind = Canon::Kind::ContextSet;
		canon.contextType.decode(r.raw());
		canon.contextIndex = r.u32();
		break;
	case 0x06:
		canon.kind = Canon::Kind::SubtaskCancel;
		canon.async = parseAsyncFlag(r);
		break;
	case 0x0D:
		canon.kind = Canon::Kind::SubtaskDrop;
		break;
	case 0x0E:
		canon.kind = Canon::Kind::StreamNew;
		canon.typeIndex = r.u32();
		break;
	case 0x0F:
		canon.kind = Canon::Kind::StreamRead;
		canon.typeIndex = r.u32();
		canon.opts = parseCanonOpts(r);
		break;
	case 0x10:
		canon.kind = Canon::Kind::StreamWrite;
		canon.typeIndex = r.u32();
		canon.opts = parseCanonOpts(r);
		break;
	case 0x11:
		canon.kind = Canon::Kind::StreamCancelRead;
		canon.typeIndex = r.u32();
		canon.async = parseAsyncFlag(r);
		break;
	case 0x12:
		canon.kind = Canon::Kind::StreamCancelWrite;
		canon.typeIndex = r.u32();
		canon.async = parseAsyncFlag(r);
		break;
	case 0x13:
		canon.kind = Canon::Kind::StreamDropReadable;
		canon.typeIndex = r.u32();
		break;
	case 0x14:
		canon.kind = Canon::Kind::StreamDropWritable;
		canon.typeIndex = r.u32();
		break;
	case 0x2E:
		canon.kind = Canon::Kind::StreamForward;
		canon.typeIndex = r.u32();
		break;
	case 0x15:
		canon.kind = Canon::Kind::FutureNew;
		canon.typeIndex = r.u32();
		break;
	case 0x16:
		canon.kind = Canon::Kind::FutureRead;
		canon.typeIndex = r.u32();
		canon.opts = parseCanonOpts(r);
		break;
	case 0x17:
		canon.kind = Canon::Kind::FutureWrite;
		canon.typeIndex = r.u32();
		canon.opts = parseCanonOpts(r);
		break;
	case 0x18:
		canon.kind = Canon::Kind::FutureCancelRead;
		canon.typeIndex = r.u32();
		canon.async = parseAsyncFlag(r);
		break;
	case 0x19:
		canon.kind = Canon::Kind::FutureCancelWrite;
		canon.typeIndex = r.u32();
		canon.async = parseAsyncFlag(r);
		break;
	case 0x1A:
		canon.kind = Canon::Kind::FutureDropReadable;
		canon.typeIndex = r.u32();
		break;
	case 0x1B:
		canon.kind = Canon::Kind::FutureDropWritable;
		canon.typeIndex = r.u32();
		break;
	case 0x2F:
		canon.kind = Canon::Kind::FutureForward;
		canon.typeIndex = r.u32();
		break;
	case 0x1C:
		canon.kind = Canon::Kind::ErrorContextNew;
		canon.opts = parseCanonOpts(r);
		break;
	case 0x1D:
		canon.kind = Canon::Kind::ErrorContextDebugMessage;
		canon.opts = parseCanonOpts(r);
		break;
	case 0x1E:
		canon.kind = Canon::Kind::ErrorContextDrop;
		break;
	case 0x1F:
		canon.kind = Canon::Kind::WaitableSetNew;
		break;
	case 0x20:
		expectZero();
		canon.kind = Canon::Kind::WaitableSetWait;
		canon.memoryIndex = r.u32();
		break;
	case 0x21:
		expectZero();
		canon.kind = Canon::Kind::WaitableSetPoll;
		canon.memoryIndex = r.u32();
		break;
	case 0x22:
		canon.kind = Canon::Kind::WaitableSetDrop;
		break;
	case 0x23:
		canon.kind = Canon::Kind::WaitableJoin;
		break;
	case 0x26:
		canon.kind = Canon::Kind::ThreadIndex;
		break;
	case 0x27:
		canon.kind = Canon::Kind::ThreadNewIndirect;
		canon.coreTypeIndex = r.u32();
		canon.tableIndex = r.u32();
		break;
	case 0x28:
		canon.kind = Canon::Kind::ThreadResumeLater;
		break;
	case 0x29:
		expectZero();
		canon.kind = Canon::Kind::ThreadSuspend;
		break;
	case 0x0C:
		expectZero();
		canon.kind = Canon::Kind::ThreadYield;
		break;
	case 0x2A:
		expectZero();
		canon.kind = Canon::Kind::ThreadSuspendThenResume;
		break;
	case 0x2B:
		expectZero();
		canon.kind = Canon::Kind::ThreadYieldThenResume;
		break;
	case 0x2C:
		expectZero();
		canon.kind = Canon::Kind::ThreadSuspendThenPromote;
		break;
	case 0x2D:
		expectZero();
		canon.kind = Canon::Kind::ThreadYieldThenPromote;
		break;
	case 0x40:
		canon.kind = Canon::Kind::ThreadSpawnRef;
		canon.shared = parseSharedFlag(r);
		canon.coreTypeIndex = r.u32();
		break;
	case 0x41:
		canon.kind = Canon::Kind::ThreadSpawnIndirect;
		canon.shared = parseSharedFlag(r);
		canon.coreTypeIndex = r.u32();
		canon.tableIndex = r.u32();
		break;
	case 0x42:
		canon.kind = Canon::Kind::ThreadAvailableParallelism;
		canon.shared = parseSharedFlag(r);
		break;
	default:
		throw std::runtime_error("component parse: invalid canonical builtin " +
				std::to_string(op));
	}
	return canon;
}
