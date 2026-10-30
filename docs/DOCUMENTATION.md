# Header Documentation — Doxygen Style, and Why It Is Not Optional

Status: policy note (revision 1 — 2026-10-29: every comment in the project's
`.h`/`.hpp` files was converted to Qt-style Doxygen comments, `Doxyfile` was
added, and `Test/HeaderDocTests.cpp` now enforces the rule; this note records the
forms, the escaping pitfalls that a real Doxygen 1.18 run exposed, and the
check).
Audience: anyone adding or editing a header, or reviewing a change that touches
one.

The headers are not a private implementation detail that happens to be visible.
`WasmBase/` is the embedder-facing API ([`API_ABI_STABILITY.md`](API_ABI_STABILITY.md)
freezes it), the backend seam is described in those headers, and the design
rationale for nearly every rule in the engine — the trap protocol, the type
identity ladder, the store ownership rule — lives in a comment above the
declaration it governs, not in a separate prose document. A comment that a
reader cannot *find* is a comment that will be duplicated, contradicted or
simply lost. This note is why the headers carry Doxygen markup and what that
markup has to look like. It contains no engine behaviour.

## 1. Document map

| Section | Question it answers |
|---|---|
| §2 | What is the rule? |
| §3 | Which comment forms are documentation, and which are not? (function and template blocks: §3.1) |
| §4 | What does every header start with? |
| §5 | Which prose breaks a Doxygen run, and how is it written instead? |
| §6 | How do I check? |
| §7 | Why is this necessary rather than nice to have? |
| §8 | What is deliberately left undone? |
| §9 | References |

## 2. The rule

**Every comment in a `.h`/`.hpp` file is a Doxygen comment**, with three
deliberate exceptions listed in §3. The rule covers the public headers, the
backends, the stub, and the test harness — the whole tree except `extern/`,
which is a submodule and is left exactly as upstream wrote it.

The corollary matters as much as the rule: a comment sits **where the thing it
describes is declared**. Prose about `Runtime` goes above `class Runtime`, not
above the config struct that happens to precede it; a paragraph about a trap
goes above the trap enum, not in a file-level preamble. When the conversion was
done, two blocks in `WasmRuntime.hpp` and `WasmAgent.hpp` were attached to the
wrong declaration — nothing caught it before, because nothing rendered the
comments.

## 3. Which comment forms are documentation

Qt's Doxygen dialect (the project is a qmake/Qt project, and the surrounding
ecosystem reads it) uses these forms:

| Form | Use |
|---|---|
| `//! text` | a doc block before a declaration |
| `//!< text` | a doc block *after* a declaration — enumerators, struct fields |
| `/*! … */` | a multi-line doc block before a declaration |
| `/*!< … */` | a multi-line doc block after a declaration |
| `\file`, `\brief` | the file header block (§4) |
| `\brief`, `\param`, `\return`, `\tparam`, `\note`, `\sa`, `\throws`, `\par` | inside a block |
| `\par Title` | a heading inside one declaration's block (a former ASCII banner) |

Three kinds of comment are **not** documentation. Doxygen never renders them,
and writing a marker there does active harm — `#endif //! GUARD` attaches the
text to whatever was documented last:

| Not documentation | Why it stays plain |
|---|---|
| `#endif // WASMTRAP_HPP`, `#if … // NOLINT` | an annotation on a preprocessor line |
| `} // namespace WASM`, `} // extern "C"` | an annotation on a closing brace |
| `bool /*async*/ handle` | a name-suppression idiom *inside* a declaration |

This is exactly the exemption `Test/HeaderDocTests.cpp` implements; anything
else in a header without a marker fails the `[headerdoc]` test.

### 3.1 Function and template documentation

**Function documentation makes use of `\brief`, `\param`, `\return` and
`\tparam`.** The same rule applies to *anything* templated — a class template,
an alias template, a variable template, a template member — every template
parameter gets a `\tparam`.

```cpp
/*!
  \brief Loads an artifact and instantiates it into `store`.
  \tparam Artifact the parsed artifact type (`Module` or `Component`)
  \param path the file to load
  \param store the store the instance's entities are owned by
  \return the instance, owned by the caller
  \throws std::runtime_error when the file does not parse
*/
template <class Artifact>
std::unique_ptr<ModuleInstance> instantiateFrom(const std::filesystem::path& path,
		Store& store);
```

The commands have rules, and each of them is a warning Doxygen emits rather
than a matter of taste:

| Command | Rule |
|---|---|
| `\brief` | one sentence, on the first line of the block; it is what every index shows |
| `\param name` | **exactly one per declared parameter, spelled exactly as declared**, in declaration order. Never document a parameter that does not exist, and never write two `\param` sections for one name |
| `\return` | only when the function returns a value. Never on `void`, a constructor, a destructor or an overloaded operator that returns nothing |
| `\tparam name` | exactly one per template parameter, spelled exactly as declared — for functions, classes, aliases and variable templates alike |
| `\throws` | the exception types the function can propagate; needs whitespace after the command (`\throws std::runtime_error …`) |

Three failure modes were found in the tree when this rule was written down, and
all three are now fixed; they are listed here because they are what a
generated or copy-pasted block tends to do:

- `\return The result.` on a `void` handler, a constructor or a destructor
  (§"found documented return type … that does not return anything");
- `\param` names guessed from the prose (`\param async` for a parameter that is
  commented out of the signature, `\param true`, `\param store` twice);
- a doc block inserted **inside** a macro body or a multi-line expression, which
  documents a token-paste helper instead of the declaration it was meant for.

`WARN_NO_PARAMDOC = YES` in the [`Doxyfile`](../Doxyfile) is what enforces the
`\param`/`\tparam` half: a documented function that omits one of its parameters
makes `doxygen Doxyfile` warn, and §6 requires it to be silent. `\return` on a
void function and a `\param` that names no parameter are reported by
`WARN_IF_DOC_ERROR`, which is on for the same reason.

A function with no doc comment at all is a *different* gap: it is not a
renderer error, and the count is tracked in §8.

## 4. Every header starts with a file block

Each header opens with a block that says what the file is, because the generated
"Files" page is the front door of the reference:

```cpp
/*!
  \file WasmTrap.hpp
  \brief The trap taxonomy and the pending-trap protocol.

  This header is part of the exported surface: every declaration in it
  is exported (docs/EMBEDDING_SURFACE.md ES-4).
*/
```

The `\brief` is one line, in the imperative or noun-phrase register the rest of
the tree uses ("The trap taxonomy and the pending-trap protocol.", not "This
file contains…"). The ES-4 note may be a `\note`, but it must be *in the file
block*: written as a plain `//` preamble above `WASMJIT_VISIBLE_BEGIN` it becomes
the file's brief (Doxygen attaches a leading comment block to the file when no
declaration follows), which is how 54 headers ended up describing themselves as
"docs/EMBEDDING_SURFACE.md ES-4: this header is entirely part of…".

## 5. Prose that breaks a Doxygen run

Running Doxygen is the only honest way to find these; the list below is what a
real 1.18 run reported on this tree, with the rewrite that fixed each one.

| Written | What Doxygen does | Written instead |
|---|---|---|
| a code span **ending in `*`**, e.g. `` `WASM.EXECUTE.TRAP.*` `` | opens `<tt>` and never closes it ("found `</tt>` tag without matching `<tt>`") | end the span before the star: `` `WASM.EXECUTE.TRAP` `` plus the star as prose |
| a period inside a code span in the **first sentence**, e.g. "One record from a `metadata.code.*` …" | `QT_AUTOBRIEF` cuts the brief at that period, inside the span | make the first sentence end on its own: "One record from a compilation-hint custom section. The family is `metadata.code.*` …" |
| a bare `#` in prose, e.g. `O(#live instances)` or `docs/VALIDATION.md#unknown-type` | `#name` is a link request ("explicit link request to 'live' could not be resolved") | write it out (`O(live instances)`), or escape it: `\#unknown-type` |
| an unescaped `<` that a reader means literally, e.g. `` `b < a ? b : a` `` | is read as markup | escape it as `\<` when it is not inside a code span, or keep it out of a brief |
| `/* … */` with the body flush left | renders, but the project style is a `/*!` block whose body is indented two spaces | `/*!` … two-space body … `*/` |
| a documented **member named `module`** | Doxygen 1.18 reads it as a C++20 module declaration ("Found documentation for module nullptr but it has no primary interface unit") | name it for what it holds (`moduleContext`) |

The `module` row is a Doxygen bug, not a naming rule; `WasmStub/` hit it and the
member carries a comment saying so. If a future Doxygen stops doing this, the
comment can go.

## 6. How to check

Three commands, all cheap:

```bash
# 1. Render the reference. Any output on stderr is a doc bug: the tree is clean.
doxygen Doxyfile
$BROWSER build/doxygen/html/index.html

# 2. Enforce the shape and the file blocks (part of the default suite).
./Test/WasmJit '[headerdoc]'

# 3. Read a header the way a newcomer will: the top of the file, then the
#    brief of its first declaration.
```

The output goes to `build/doxygen/` — never into the source tree, like every
other artefact (`AGENTS.md`).

## 7. Why this is necessary

1. **The headers are the API reference.** There is no hand-written reference
   manual and there should not be one: it would be a second copy of the same
   facts, updated by whoever remembers. `docs/` carries the *design reasoning*;
   the headers carry the *contract*. If the contract is not rendered, an
   embedder's only option is to read the source, and a review can only check
   what a reviewer happens to open.
2. **A prose-only rule drifts, and this tree has the receipts.**
   [`LAYERING.md`](LAYERING.md) exists because 48 comment sites in `WasmBase/`
   had quietly started speaking in one backend's vocabulary. Nothing failed —
   comments cannot fail a build — so the drift survived until someone wrote a
   policy down. The `[headerdoc]` test is the same lesson applied *before* the
   drift: the rule is checked by the suite, not by attention.
3. **It makes the missing documentation visible.** With `EXTRACT_ALL` and a
   clean run, the reference shows every declaration, documented or not. The gaps
   are then a deliberate list (§8) instead of an invisible part of the tree.
4. **It is what the tools in the project already assume.** The C++ ABI and the
   plugin descriptor are read by people and by tooling; a header that renders is
   a header a plugin author can navigate. [`BACKEND_WORK_ORDER.md`](BACKEND_WORK_ORDER.md)
   points a new backend at these headers as its contract.

## 8. What is deliberately not done

- **Not every declaration is documented *yet*.** The rule above is about the
  shape and the completeness of a block that exists; a declaration with no block
  at all remains a gap. Measured on 2026-10-30 with `EXTRACT_ALL=NO` +
  `WARN_IF_UNDOCUMENTED=YES`: **896 declarations** (458 functions, 365 fields and
  constants, 54 macros, 7 enums, 4 typedefs, 7 compounds). That is down from
  **4,591** before the sweep, and it is a list to work through, not a decision to
  stop: a new or edited declaration is expected to arrive documented, and the
  count must not grow.
- **A doc block must say something.** A block of the form
  "`\brief Performs the <name> operation. \param x Input value or output
  destination for this operation.`" is *worse than no block*: it renders, it is
  counted as documented, and it states nothing (or, on a `void` handler, states
  something false). Such blocks were removed from the tree in the same pass that
  fixed the renderer warnings; do not reintroduce them.
- **`extern/` is untouched.** It is a submodule; reformatting it would make the
  next submodule update a merge.
- **`.cpp` files are untouched.** The rule is about the surface a reader is
  handed, which is the headers. Implementation comments keep their `//` style
  (they are also where `Test/LayeringTests.cpp`-style checks look for prose).

## 9. References

- [`Doxyfile`](../Doxyfile) — the renderer's configuration; `QT_AUTOBRIEF`,
  `WARN_NO_PARAMDOC` and the `build/doxygen` output directory are asserted by the
  `[headerdoc]` test.
- [`Test/HeaderDocTests.cpp`](../Test/HeaderDocTests.cpp) — the check.
- [`LAYERING.md`](LAYERING.md) §6 — the sibling rule: enforce the invariant,
  do not rely on review.
- [`API_ABI_STABILITY.md`](API_ABI_STABILITY.md) — the public headers that make
  the reference an API contract.
- [`EMBEDDING_SURFACE.md`](EMBEDDING_SURFACE.md) §6 — the header inventory the
  file blocks belong to.

