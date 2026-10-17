#include "LibJitContext.hpp"

namespace LibJIT {
Context::Context()
	: context(jit_context_create())
{

}

Context::~Context()
{
	jit_context_destroy(context);
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
