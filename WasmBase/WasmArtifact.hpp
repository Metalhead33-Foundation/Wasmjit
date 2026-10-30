#ifndef WASMARTIFACT_HPP
#define WASMARTIFACT_HPP

#include "WasmApi.hpp"

/*!
  \file WasmArtifact.hpp
  \brief Artifact kind detection, the preamble and the parse entry point.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/

WASMJIT_VISIBLE_BEGIN

#include "WasmApi.hpp"
#include "WasmSection.hpp"
#include <cstdint>
#include <memory>
#include <optional>

namespace WASM {

/** \\brief Forward declaration of the parsed core-module artifact. */
class Module;
/** \\brief Forward declaration of the parsed Component Model artifact. */
class Component;
/** \\brief Forward declaration of the Store that owns canonical artifact types. */
class Store;

//! The two kinds of artifact that share the `\0asm` magic.
/** \brief Kind of WebAssembly artifact identified by the preamble. */
enum class ArtifactKind : uint8_t {
	CoreModule, //!< Core WebAssembly module.
	Component  //!< Component Model component.
};

//! Component Model preamble values (the core module version is not checked).
inline constexpr uint16_t kComponentVersion = 0x000D;
inline constexpr uint16_t kComponentLayer = 0x0001;

/** \brief Version and layer fields read from an artifact's eight-byte preamble. */
struct Preamble {
	uint32_t version = 0; //!< Encoded format version.
	uint16_t layer = 0;   //!< Encoded layer discriminator.
};

//! Common base for a parsed core module and a parsed Component Model component.
//! `Artifact::parse` is the "parse this file" entry point: it reads the first
//! eight bytes and dispatches on the layer field, so a caller does not need to
//! know in advance which kind it holds.
/**
 * \brief Polymorphic base for parsed core modules and Component Model artifacts.
 *
 * Use parse() to inspect the preamble and construct the matching derived type.
 * The artifact is associated with the Store supplied during parsing; that
 * store must outlive the artifact.
 */
struct WASMJIT_API Artifact {
	ArtifactKind kind; //!< Field `kind` of WASMJIT_API.
	uint32_t version = 0; //!< Field `version` of WASMJIT_API.
	uint16_t layer = 0; //!< Field `layer` of WASMJIT_API.

	/** \brief Constructs an artifact with its parsed kind. \param artifactKind Core-module or component kind. */
	explicit Artifact(ArtifactKind artifactKind) : kind(artifactKind) {}
	virtual ~Artifact() = default;

	Artifact(const Artifact&) = default;
	Artifact& operator=(const Artifact&) = default;

	/** \brief Tests whether this artifact is a core module. \return true for CoreModule. */
	bool isCoreModule() const { return kind == ArtifactKind::CoreModule; }
	/** \brief Tests whether this artifact is a component. \return true for Component. */
	bool isComponent() const { return kind == ArtifactKind::Component; }

	//! Downcasts; null when the artifact is the other kind.
	/** \brief Attempts a mutable core-module downcast. \return Core module pointer, or nullptr for a component. */
	Module* asCoreModule();
	/** \brief Attempts a read-only core-module downcast. \return Core module pointer, or nullptr for a component. */
	const Module* asCoreModule() const;
	/** \brief Attempts a mutable component downcast. \return Component pointer, or nullptr for a core module. */
	Component* asComponent();
	/** \brief Attempts a read-only component downcast. \return Component pointer, or nullptr for a core module. */
	const Component* asComponent() const;

	//! Reads magic (4) + version (2) + layer (2) and validates the magic.
	/**
	 * \brief Reads and validates the artifact preamble.
	 * \param file Input device positioned at the start of the artifact.
	 * \return Decoded version and layer fields.
	 * \throws Parsing/IO exceptions if the magic is invalid or the preamble
	 *         cannot be read completely.
	 */
	static Preamble readPreamble(Elv::Io::Device& file);

	//! Reads the preamble and returns a Module or a Component, interning every
	//! type in `store`. An artifact belongs to exactly one store; there is no
	//! process-wide default (docs/EMBEDDING_SURFACE.md §4.3 option B).
	/**
	 * \brief Parses an artifact and interns its types in the supplied store.
	 * \param file Input device positioned at the artifact preamble.
	 * \param store Store that owns the parsed artifact's canonical types; it
	 *        must outlive the returned artifact.
	 * \return Owning pointer to the parsed core module or component.
	 * \throws Parsing/validation exceptions when the preamble or body is invalid.
	 */
	static std::unique_ptr<Artifact> parse(Elv::Io::Device& file, Store& store);

	//! The store this artifact was parsed into. Valid after `parse`/`fromFile`.
	/** \brief Returns the store that owns this artifact's canonical types. \return Reference to the associated store. */
	Store& store() const { return *store_; }

protected:
	//! Decodes the body after the preamble. `end` bounds a nested body inside
	//! a component; `std::nullopt` means "read to the device's end".
	/**
	 * \brief Parses the artifact body after its preamble.
	 * \param file Input device containing the body.
	 * \param end Optional absolute end offset for a nested body; nullopt means
	 *        parse through the device's end.
	 */
	virtual void parseBody(Elv::Io::Device& file, std::optional<uint64_t> end) = 0;

	Store* store_ = nullptr; //!< Field `store_` of WASMJIT_API.
};

}

WASMJIT_VISIBLE_END
#endif // WASMARTIFACT_HPP