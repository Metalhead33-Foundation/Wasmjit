#include "WasmTypeRegistry.hpp"
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

namespace WASM {

std::span<const Subtype> TypeRegistry::registerModule(std::vector<Subtype> types)
{
	if (types.empty())
		return {};

	// Move the vector into a heap block, then take a span over the block's
	// buffer. The block pointer is stable, so the span remains valid.
	auto block = std::make_unique<TypeBlock>(std::move(types));
	totalTypes  += block->size();
	parsedTypes += block->size();
	std::span<const Subtype> view(*block);
	blocks.push_back(std::move(block));
	return view;
}

namespace {

CanonHeapType toCanonHeapType(int32_t heapType, uint32_t groupFirst, uint32_t groupEnd,
							  const std::vector<TypeId>& localToGlobal)
{
	CanonHeapType out;
	if (heapType < 0) {
		out.kind = CanonHeapType::Kind::Abstract;
		out.abstract = static_cast<AbstractHeapType>(heapType);
		return out;
	}
	const uint32_t idx = static_cast<uint32_t>(heapType);
	if (idx >= groupFirst && idx < groupEnd) {
		out.kind = CanonHeapType::Kind::Rec;
		out.memberIndex = idx - groupFirst;
	} else {
		out.kind = CanonHeapType::Kind::Global;
		out.id = localToGlobal[idx];
	}
	return out;
}

// Shorthand reference value types (`funcref`, `externref`, ...) are
// `(ref null <abstract>)`; normalize so both spellings share an identity.
bool shorthandRefHeap(ValueTypeCode code, AbstractHeapType& heap)
{
	switch (code) {
	case ValueTypeCode::FuncRef:       heap = AbstractHeapType::Func;     return true;
	case ValueTypeCode::ExternRef:     heap = AbstractHeapType::Extern;   return true;
	case ValueTypeCode::AnyRef:        heap = AbstractHeapType::Any;      return true;
	case ValueTypeCode::EqRef:         heap = AbstractHeapType::Eq;       return true;
	case ValueTypeCode::I31Ref:        heap = AbstractHeapType::I31;      return true;
	case ValueTypeCode::StructRef:     heap = AbstractHeapType::Struct;   return true;
	case ValueTypeCode::ArrayRef:      heap = AbstractHeapType::Array;    return true;
	case ValueTypeCode::NullFuncRef:   heap = AbstractHeapType::NoFunc;   return true;
	case ValueTypeCode::NullExternRef: heap = AbstractHeapType::NoExtern; return true;
	case ValueTypeCode::NullRef:       heap = AbstractHeapType::None;     return true;
	default: return false;
	}
}

CanonValueType toCanonValueType(const ValueType& vt, uint32_t groupFirst, uint32_t groupEnd,
								const std::vector<TypeId>& localToGlobal)
{
	CanonValueType out;
	AbstractHeapType shorthand{};
	if (shorthandRefHeap(vt.opcode, shorthand)) {
		out.opcode = ValueTypeCode::RefNull;
		out.nullable = true;
		out.heap.kind = CanonHeapType::Kind::Abstract;
		out.heap.abstract = shorthand;
		return out;
	}
	out.opcode = vt.opcode;
	out.nullable = (vt.opcode == ValueTypeCode::RefNull);
	if (vt.opcode == ValueTypeCode::Ref || vt.opcode == ValueTypeCode::RefNull)
		out.heap = toCanonHeapType(vt.heapType, groupFirst, groupEnd, localToGlobal);
	return out;
}

CanonStorageType toCanonStorage(const StorageType& st, uint32_t groupFirst, uint32_t groupEnd,
								const std::vector<TypeId>& localToGlobal)
{
	CanonStorageType out;
	if (st.isPacked) {
		out.isPacked = true;
		out.packed = st.val.opcode;
	} else {
		out.value = toCanonValueType(st.val, groupFirst, groupEnd, localToGlobal);
	}
	return out;
}

std::vector<CanonicalType> buildGroup(std::span<const Subtype> types, const TypeGroup& group,
									  const std::vector<TypeId>& localToGlobal, uint32_t groupEnd)
{
	const uint32_t groupFirst = group.first.value;
	std::vector<CanonicalType> out;
	out.reserve(group.count);
	for (uint32_t i = 0; i < group.count; ++i) {
		const Subtype& st = types[groupFirst + i];
		CanonicalType ct;
		ct.isFinal = st.isFinal;
		for (uint32_t s : st.supertypeIndices)
			ct.supertypes.push_back(
				toCanonHeapType(static_cast<int32_t>(s), groupFirst, groupEnd, localToGlobal));
		if (st.isFunction()) {
			ct.kind = CanonTypeKind::Func;
			const FuncType& ft = std::get<FuncType>(st.composite);
			for (const StorageType& p : ft.params)
				ct.funcParams.push_back(toCanonStorage(p, groupFirst, groupEnd, localToGlobal));
			for (const StorageType& r : ft.results)
				ct.funcResults.push_back(toCanonStorage(r, groupFirst, groupEnd, localToGlobal));
		} else if (st.isStruct()) {
			ct.kind = CanonTypeKind::Struct;
			for (const FieldType& f : std::get<StructType>(st.composite).fields) {
				CanonFieldType cf;
				cf.storage = toCanonStorage(f.storageType, groupFirst, groupEnd, localToGlobal);
				cf.isMutable = f.isMutable;
				ct.structFields.push_back(std::move(cf));
			}
		} else if (st.isArray()) {
			ct.kind = CanonTypeKind::Array;
			const ArrayType& at = std::get<ArrayType>(st.composite);
			ct.arrayElement.storage =
				toCanonStorage(at.elementType.storageType, groupFirst, groupEnd, localToGlobal);
			ct.arrayElement.isMutable = at.elementType.isMutable;
		}
		out.push_back(std::move(ct));
	}
	return out;
}

// --- canonical serialization (an injective key for interning) ---
void appendU8(std::string& s, uint8_t v) { s.push_back(static_cast<char>(v)); }
void appendU32(std::string& s, uint32_t v) {
	for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}
void appendI32(std::string& s, int32_t v) { appendU32(s, static_cast<uint32_t>(v)); }

void serializeHeap(std::string& s, const CanonHeapType& h) {
	appendU8(s, static_cast<uint8_t>(h.kind));
	switch (h.kind) {
	case CanonHeapType::Kind::Abstract: appendI32(s, static_cast<int32_t>(h.abstract)); break;
	case CanonHeapType::Kind::Global:   appendU32(s, h.id.value); break;
	case CanonHeapType::Kind::Rec:      appendU32(s, h.memberIndex); break;
	}
}
void serializeValue(std::string& s, const CanonValueType& v) {
	appendI32(s, static_cast<int32_t>(v.opcode));
	if (v.opcode == ValueTypeCode::Ref || v.opcode == ValueTypeCode::RefNull) {
		appendU8(s, v.nullable ? 1 : 0);
		serializeHeap(s, v.heap);
	}
}
void serializeStorage(std::string& s, const CanonStorageType& st) {
	if (st.isPacked) { appendU8(s, 1); appendI32(s, static_cast<int32_t>(st.packed)); }
	else { appendU8(s, 0); serializeValue(s, st.value); }
}
void serializeField(std::string& s, const CanonFieldType& f) {
	serializeStorage(s, f.storage);
	appendU8(s, f.isMutable ? 1 : 0);
}
void serializeType(std::string& s, const CanonicalType& t) {
	appendU8(s, static_cast<uint8_t>(t.kind));
	appendU8(s, t.isFinal ? 1 : 0);
	appendU32(s, static_cast<uint32_t>(t.supertypes.size()));
	for (const CanonHeapType& h : t.supertypes) serializeHeap(s, h);
	if (t.kind == CanonTypeKind::Func) {
		appendU32(s, static_cast<uint32_t>(t.funcParams.size()));
		for (const CanonStorageType& p : t.funcParams) serializeStorage(s, p);
		appendU32(s, static_cast<uint32_t>(t.funcResults.size()));
		for (const CanonStorageType& r : t.funcResults) serializeStorage(s, r);
	} else if (t.kind == CanonTypeKind::Struct) {
		appendU32(s, static_cast<uint32_t>(t.structFields.size()));
		for (const CanonFieldType& f : t.structFields) serializeField(s, f);
	} else {
		serializeField(s, t.arrayElement);
	}
}
std::string serializeGroup(const std::vector<CanonicalType>& group) {
	std::string s;
	appendU32(s, static_cast<uint32_t>(group.size()));
	for (const CanonicalType& t : group) serializeType(s, t);
	return s;
}

void remapStorage(CanonStorageType& st, TypeId base) {
	if (st.isPacked) return;
	CanonValueType& v = st.value;
	if (v.opcode == ValueTypeCode::Ref || v.opcode == ValueTypeCode::RefNull) {
		if (v.heap.kind == CanonHeapType::Kind::Rec) {
			v.heap.kind = CanonHeapType::Kind::Global;
			v.heap.id = TypeId{base.value + v.heap.memberIndex};
		}
	}
}

std::vector<CanonicalType> resolveGroup(const std::vector<CanonicalType>& pending, TypeId base,
										const std::vector<CanonicalType>& canonicalTypes)
{
	std::vector<CanonicalType> out = pending; // deep copy
	std::vector<uint32_t> depths(out.size(), 0);
	for (size_t i = 0; i < out.size(); ++i) {
		uint32_t depth = 0;
		for (const CanonHeapType& s : out[i].supertypes) {
			uint32_t sd = 0;
			if (s.kind == CanonHeapType::Kind::Rec) sd = depths[s.memberIndex];
			else if (s.kind == CanonHeapType::Kind::Global) sd = canonicalTypes[s.id.value].depth;
			if (sd + 1 > depth) depth = sd + 1;
		}
		out[i].depth = depth;
		depths[i] = depth;

		for (CanonHeapType& s : out[i].supertypes) {
			if (s.kind == CanonHeapType::Kind::Rec) {
				s.kind = CanonHeapType::Kind::Global;
				s.id = TypeId{base.value + s.memberIndex};
			}
		}
		for (CanonStorageType& p : out[i].funcParams) remapStorage(p, base);
		for (CanonStorageType& r : out[i].funcResults) remapStorage(r, base);
		for (CanonFieldType& f : out[i].structFields) remapStorage(f.storage, base);
		remapStorage(out[i].arrayElement.storage, base);
	}
	return out;
}

// --- declared-subtype / structural matching (M4; reused by M5) ---
struct CanonView {
	const std::vector<CanonicalType>* earlier = nullptr;
	const std::vector<CanonicalType>* group = nullptr; // group being interned, if any
	TypeId groupBase{};
	const CanonicalType& type(TypeId id) const {
		if (group != nullptr && id.value >= groupBase.value)
			return (*group)[id.value - groupBase.value];
		return (*earlier)[id.value];
	}
};

bool isInternalAbstract(AbstractHeapType t) {
	switch (t) {
	case AbstractHeapType::Any: case AbstractHeapType::Eq: case AbstractHeapType::I31:
	case AbstractHeapType::Struct: case AbstractHeapType::Array: case AbstractHeapType::None:
		return true;
	default: return false;
	}
}

// `a` is a subtype of `b` within the abstract heap-type lattice.
bool abstractMatches(AbstractHeapType a, AbstractHeapType b) {
	if (a == b) return true;
	if (a == AbstractHeapType::None)     return isInternalAbstract(b);
	if (a == AbstractHeapType::NoFunc)   return b == AbstractHeapType::Func;
	if (a == AbstractHeapType::NoExtern) return b == AbstractHeapType::Extern;
	if (a == AbstractHeapType::I31 || a == AbstractHeapType::Struct || a == AbstractHeapType::Array)
		return b == AbstractHeapType::Eq || b == AbstractHeapType::Any;
	if (a == AbstractHeapType::Eq) return b == AbstractHeapType::Any;
	return false;
}

bool concreteMatchesAbstract(const CanonView& v, TypeId id, AbstractHeapType abs) {
	const CanonicalType& t = v.type(id);
	switch (abs) {
	case AbstractHeapType::Any:
	case AbstractHeapType::Eq:
		return t.kind == CanonTypeKind::Struct || t.kind == CanonTypeKind::Array;
	case AbstractHeapType::Struct: return t.kind == CanonTypeKind::Struct;
	case AbstractHeapType::Array:  return t.kind == CanonTypeKind::Array;
	case AbstractHeapType::Func:   return t.kind == CanonTypeKind::Func;
	default: return false;
	}
}

// Nominal subtyping between concrete types: equality or a declared supertype.
bool concreteMatches(const CanonView& v, TypeId a, TypeId b) {
	if (a == b) return true;
	std::unordered_set<uint32_t> seen;
	std::vector<TypeId> stack{a};
	seen.insert(a.value);
	while (!stack.empty()) {
		const TypeId cur = stack.back();
		stack.pop_back();
		for (const CanonHeapType& s : v.type(cur).supertypes) {
			if (s.kind != CanonHeapType::Kind::Global) continue;
			if (s.id == b) return true;
			if (seen.insert(s.id.value).second) stack.push_back(s.id);
		}
	}
	return false;
}

bool heapMatches(const CanonView& v, const CanonHeapType& a, const CanonHeapType& b) {
	if (a.kind == CanonHeapType::Kind::Abstract)
		return b.kind == CanonHeapType::Kind::Abstract && abstractMatches(a.abstract, b.abstract);
	if (a.kind == CanonHeapType::Kind::Global) {
		if (b.kind == CanonHeapType::Kind::Abstract) return concreteMatchesAbstract(v, a.id, b.abstract);
		if (b.kind == CanonHeapType::Kind::Global)   return concreteMatches(v, a.id, b.id);
	}
	return false;
}

bool isRefOpcode(ValueTypeCode c) {
	return c == ValueTypeCode::Ref || c == ValueTypeCode::RefNull;
}

bool valueMatches(const CanonView& v, const CanonValueType& a, const CanonValueType& b) {
	if (isRefOpcode(a.opcode) || isRefOpcode(b.opcode)) {
		if (!isRefOpcode(a.opcode) || !isRefOpcode(b.opcode)) return false;
		if (a.nullable && !b.nullable) return false;
		return heapMatches(v, a.heap, b.heap);
	}
	return a.opcode == b.opcode;
}

bool storageMatches(const CanonView& v, const CanonStorageType& a, const CanonStorageType& b) {
	if (a.isPacked != b.isPacked) return false;
	if (a.isPacked) return a.packed == b.packed;
	return valueMatches(v, a.value, b.value);
}

bool fieldMatches(const CanonView& v, const CanonFieldType& a, const CanonFieldType& b) {
	if (a.isMutable != b.isMutable) return false;
	if (a.isMutable) return a.storage == b.storage; // mutable fields are invariant
	return storageMatches(v, a.storage, b.storage);
}

// Structural comptype subtyping used to validate `sub` annotations: `actual` is
// the subtype, `expected` the declared supertype's comptype.
bool compMatches(const CanonView& v, const CanonicalType& actual, const CanonicalType& expected) {
	if (actual.kind != expected.kind) return false;
	switch (actual.kind) {
	case CanonTypeKind::Func: {
		if (actual.funcParams.size() != expected.funcParams.size()) return false;
		if (actual.funcResults.size() != expected.funcResults.size()) return false;
		for (size_t i = 0; i < actual.funcParams.size(); ++i)
			if (!storageMatches(v, expected.funcParams[i], actual.funcParams[i])) return false; // contravariant
		for (size_t i = 0; i < actual.funcResults.size(); ++i)
			if (!storageMatches(v, actual.funcResults[i], expected.funcResults[i])) return false; // covariant
		return true;
	}
	case CanonTypeKind::Struct: {
		if (actual.structFields.size() < expected.structFields.size()) return false; // width
		for (size_t i = 0; i < expected.structFields.size(); ++i)
			if (!fieldMatches(v, actual.structFields[i], expected.structFields[i])) return false;
		return true;
	}
	case CanonTypeKind::Array:
		return fieldMatches(v, actual.arrayElement, expected.arrayElement);
	}
	return false;
}

} // namespace

std::vector<TypeId> TypeRegistry::internModule(std::span<const Subtype> types,
											   std::span<const TypeGroup> groups)
{
	std::vector<TypeId> localToGlobal(types.size());

	for (const TypeGroup& group : groups) {
		const uint32_t groupFirst = group.first.value;
		const uint32_t groupEnd = groupFirst + group.count;

		std::vector<CanonicalType> pending = buildGroup(types, group, localToGlobal, groupEnd);
		std::string key = serializeGroup(pending);

		TypeId base{};
		auto it = internTable.find(key);
		if (it != internTable.end()) {
			base = recGroups[it->second.value].first;
		} else {
			base = TypeId{nextTypeId};
			std::vector<CanonicalType> resolved = resolveGroup(pending, base, canonicalTypes);

			// M4: declared-subtype validation (docs/TYPE_IDENTITY.md §6.5). Every
			// `sub` annotation must name a non-final supertype whose comptype the
			// subtype structurally matches. Validation happens before the group is
			// interned, so an invalid group never enters the intern table.
			const CanonView view{&canonicalTypes, &resolved, base};
			for (const CanonicalType& member : resolved) {
				for (const CanonHeapType& super : member.supertypes) {
					if (super.kind != CanonHeapType::Kind::Global)
						throw std::runtime_error("type section: unresolved supertype reference");
					const CanonicalType& superType = view.type(super.id);
					if (superType.isFinal)
						throw std::runtime_error("type section: a supertype must not be final");
					if (!compMatches(view, member, superType))
						throw std::runtime_error(
							"type section: invalid `sub` annotation (comptype does not match its supertype)");
				}
			}

			for (CanonicalType& ct : resolved)
				canonicalTypes.push_back(std::move(ct));
			nextTypeId += group.count;
			const RecGroupId groupId{static_cast<uint32_t>(recGroups.size())};
			recGroups.push_back(CanonRecGroup{base, group.count});
			internTable.emplace(std::move(key), groupId);
		}

		for (uint32_t i = 0; i < group.count; ++i)
			localToGlobal[groupFirst + i] = TypeId{base.value + i};
	}

	return localToGlobal;
}

bool TypeRegistry::matches(TypeId actual, TypeId expected) const
{
	CanonHeapType a;
	a.kind = CanonHeapType::Kind::Global;
	a.id = actual;
	CanonHeapType b;
	b.kind = CanonHeapType::Kind::Global;
	b.id = expected;
	return matchesHeap(a, b);
}

bool TypeRegistry::matchesHeap(CanonHeapType actual, CanonHeapType expected) const
{
	const CanonView view{&canonicalTypes, nullptr, TypeId{0}};
	return heapMatches(view, actual, expected);
}

} // namespace WASM
