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

LibJitTypeTranslator& Context::getTranslator()
{
	return typeTranslator;
}

const LibJitTypeTranslator& Context::getTranslator() const
{
	return typeTranslator;
}

}