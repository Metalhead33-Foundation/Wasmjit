#include "LibJitContext.hpp"

namespace LibJIT {
Context::Context()
	: context(jit_context_create())
{

}

Context::~Context()
{
	// A failed compile poisons libjit's builder; `jit_context_destroy` then
	// segfaults, so the context is deliberately leaked (LF-2 `F1`).
	if (!abandoned)
		jit_context_destroy(context);
}

void Context::abandon() noexcept
{
	abandoned = true;
}

jit_context_t Context::rawContext() const
{
	return context;
}

LibJitTypeTranslator& Context::getTranslator()
{
	return typeTranslator;
}

const LibJitTypeTranslator& Context::getTranslator() const
{
	return typeTranslator;
}

}
