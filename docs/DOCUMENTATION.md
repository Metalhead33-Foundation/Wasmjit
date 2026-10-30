# Documentation — Doxygen Style in the Headers, and the Note Tree {#doc_documentation}

Status: policy note (revision 2 — 2026-10-30: the Markdown notes are now part of
the generated site, so the 55 notes gain stable page ids and two index pages put
them into a hierarchy; the escaping list in §5 gains the Markdown rows a real
run exposed, and §9 is new. Revision 1 — 2026-10-29: every comment in the
project's `.h`/`.hpp` files was converted to Qt-style Doxygen comments,
`Doxyfile` was added, and `Test/HeaderDocTests.cpp` now enforces the rule).
Audience: anyone adding or editing a header or a note, or reviewing a change
that touches one.

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
| §9 | How are the notes arranged into a tree instead of a flat list? |
| §10 | References |

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

`WARN_NO_PARAMDOC = YES` in the [`Doxyfile`](../Doxyfile) states the intent, but it
is **not** what a default run measures: the reference is built with
`EXTRACT_ALL = YES` (so the HTML shows every declaration), and Doxygen switches
the undocumented-parameter warnings off in that mode. The check that actually
enforces §3.1 is the *strict pass* in §6, which flips `EXTRACT_ALL` off and
`WARN_IF_UNDOCUMENTED` on. `\return` on a void function and a `\param` that names
no parameter are reported by `WARN_IF_DOC_ERROR` in both modes, so those two are
caught by the ordinary build.

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

The next five rows only bite once Markdown files are rendered (they were found
when the notes joined the site), and each one is silent in a Markdown preview —
which is why they are listed rather than left to intuition:

| Written | What Doxygen does | Written instead |
|---|---|---|
| a **code span split across two lines**, or any line with an odd number of backticks (a test name broken over a line boundary) | opens a verbatim block and searches for a closing backtick to the end of the file ("Reached end of file while still searching closing backtick of a verbatim block"), swallowing the rest of the page | keep each span on one line, and keep the backtick count on a line even; `GC.md` and `TRAP_CLEANUP.md` each had one such split |
| a member-style name in plain prose, e.g. `O(#functions)` | the `#` starts a link request, exactly as in a header comment ("explicit link request to 'functions' could not be resolved") | keep it inside a code span (as here), or escape the hash with a backslash when it sits in prose |
| a bare angle-bracket placeholder in prose, even inside a code span, e.g. a quoted boilerplate block | "Unsupported xml/html tag \<name> found" | escape the angle bracket by writing a backslash immediately before it |
| a **relative link to a file outside the `Doxyfile`'s `INPUT`**, e.g. `[AGENTS.md](../AGENTS.md)` or a link to `Sljit/README.md` | "unable to resolve reference to … for \ref command": the target is not a page, so the link cannot be made | name it as inline code (`AGENTS.md`) instead of linking it |
| an anchor link whose heading carries no explicit label, e.g. `[note](#environment-notes)` | the anchor is not a Doxygen label, so the reference cannot be resolved | put a label on the heading (`{#environment-notes}`) and link to that |

**Rule of thumb:** the generated site is the arbiter, not the Markdown preview.
`doxygen Doxyfile` must report nothing; a warning is a doc bug with a rewrite,
which is why the check in §6 is the first of the four commands.

## 6. How to check

Five commands, all cheap:

```bash
# 1. Render the reference. Any output on stderr is a doc bug: the tree is clean.
doxygen Doxyfile
$BROWSER build/doxygen/html/index.html

# 2. Enforce the shape and the file blocks (part of the default suite).
./Test/WasmJit '[headerdoc]'

# 3. Enforce the note tree: every note has an id and a row in its folder index.
./Test/WasmJit '[docs]'

# 4. The strict pass: measures the two things the default render cannot, because
#    EXTRACT_ALL switches them off — a declaration with no block at all, and a
#    documented function whose parameters are not all documented (§3.1).
sed -e 's/^EXTRACT_ALL .*/EXTRACT_ALL = NO/' \
    -e 's/^WARN_IF_UNDOCUMENTED .*/WARN_IF_UNDOCUMENTED = YES/' \
    -e 's|^OUTPUT_DIRECTORY .*|OUTPUT_DIRECTORY = build/doxygen-strict|' \
    Doxyfile | doxygen -

# 5. Read a header the way a newcomer will: the top of the file, then the
#    brief of its first declaration.
```

The output goes to `build/doxygen/` (and `build/doxygen-strict/` for the strict
pass) — never into the source tree, like every other artefact (`AGENTS.md`).
The strict pass is a *work list*, not a gate: today it reports the gap counted in
§8 plus the parameter gaps that come with it.

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

- **Every declaration is now documented.** Measured on 2026-10-30 with the strict
  pass (§6): **0 undocumented declarations** across all 69 headers, down from
  **4,591** before the sweep. The count must stay at zero: a new or edited
  declaration is expected to arrive documented, and the strict pass is the check
  that catches one that does not.
- **A doc block must say something.** A block of the form
  "`\brief Performs the \<name> operation. \param x Input value or output destination for this operation.`"
  is *worse than no block*: it renders, it is
  counted as documented, and it states nothing (or, on a `void` handler, states
  something false). Such blocks were removed from the tree in the same pass that
  fixed the renderer warnings, and the trailing placeholders that survived that
  pass — ``//!< Field `x` of y``, ``//!< Enum member `x` ``,
  ``//!< Type alias `x` names the corresponding implementation/API type`` — were
  replaced with what the declaration actually is (or deleted where a `\brief`
  block directly above already said it). Do not reintroduce any of them: the
  strict pass counts such a block as documentation, so nothing fails.
- **Uniform per-opcode tails are acceptable.** The `dispatch<Op>` declarations of
  a backend carry ``//!< Lower this opcode into the backend IR.`` (a base class
  says what its default does instead). It is one sentence repeated across the
  opcode surface rather than per-opcode prose: the opcode is already named by
  the declaration, and the sentence is true of every one of them.
- **`extern/` is untouched.** It is a submodule; reformatting it would make the
  next submodule update a merge.
- **`.cpp` files are untouched.** The rule is about the surface a reader is
  handed, which is the headers. Implementation comments keep their `//` style
  (they are also where `Test/LayeringTests.cpp`-style checks look for prose).

## 9. The note tree

The site renders the Markdown notes as pages, and `GENERATE_TREEVIEW = YES` shows
them in the sidebar. Doxygen nests a page only when another page names it with
`\subpage`; without that every note is a *sibling* at the top level, which is what
a flat tree is. The hierarchy is therefore built by three index pages:

| Index page | Id | Children |
|---|---|---|
| [`index.md`](index.md) | `docs_index` | the 21 notes in `docs/`, `proposals` **and** `languages` |
| [`proposals/index.md`](proposals/index.md) | `proposals_index` | the 34 proposal notes |
| [`languages/index.md`](languages/index.md) | `languages_index` | the 4 language-study notes of `LF-1` |

A note declares its own id on the title line, and its folder's index lists that
id in a table row:

```md
# Running the unit tests {#doc_testing}                          ← docs/TESTING.md
| \subpage doc_testing | Building and running the suite, … |    ← docs/index.md
```

which produces:

```
WasmJit            (README.md, the main page)
├── Direction, Feature completeness, …   (the README's own sections)
└── docs                                 (docs/index.md)
    ├── ABI, API/ABI stability, …        (21 notes, each with its own sections)
    ├── languages                        (docs/languages/index.md)
    │   └── Triage, Clustering, Charters, Evidence   (4 study notes)
    └── proposals                        (docs/proposals/index.md)
        └── GC, SIMD, THREADS, …         (34 notes)
```

**Adding a note** takes three steps: create the file; append `{#doc_what_it_is}`
(or `{#prop_what_it_is}`, or `{#lf_what_it_is}` for the study folder) to its
`# Title` line; add one row to its folder's index. Moving a note between the
folders means changing both the id prefix and the row.

**A new folder** takes one more: a folder without an index page is not part of
the tree, so it gets `<folder>/index.md` with its own id, a `\subpage` row in
`docs/index.md`, and an entry in the `[docs]` check below. `docs/languages/` was
added that way, which is why the check now knows three index pages.

**Why the ids are explicit.** Doxygen derives an id from the path when it has to
(`md_docs_2TESTING`): it changes when the file moves and reads like a hash. An
explicit `{#doc_testing}` is stable, greppable, and is what a cross-note link can
target (`[the tree](DOCUMENTATION.md#doc_documentation)`, `[GC](../proposals/GC.md)`
style links keep working because the file links resolve to the page).

**Enforced, not remembered.** `./Test/WasmJit '[docs]'` fails when

- a note in `docs/`, `docs/proposals/` or `docs/languages/` has no `{#id}` on its
  title line;
- a note is missing from its folder's index — which is the silent failure: it
  would still build, as a sibling of the whole sub-tree;
- an index names an id that no note declares (a typo Doxygen would drop without
  complaint).

`[headerdoc]` keeps the Doxyfile's settings honest (`QT_AUTOBRIEF`,
`WARN_NO_PARAMDOC`, the output directory); the note tree adds no setting of its
own beyond the ones that turn Markdown on: `FILE_PATTERNS = … *.md` with
`README.md` and `docs` in `INPUT` (`RECURSIVE = YES`, so `docs/proposals` and
`docs/languages` are both reached), and `USE_MDFILE_AS_MAINPAGE = README.md`,
which is what makes the README the root page rather than another sibling.

## 10. References

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

