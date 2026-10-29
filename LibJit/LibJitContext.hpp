#ifndef LIBJITCONTEXT_HPP
#define LIBJITCONTEXT_HPP
#include "LibJitTypeTranslation.hpp"

/*!
  \file LibJitContext.hpp
  \brief The compilation context and the type translator it owns.

  This header is internal: it is not part of the embedding surface.
*/

namespace LibJIT {
class Context
{
private:
	jit_context_t context;
	LibJitTypeTranslator typeTranslator;
public:
	Context();
	~Context();
	jit_context_t rawContext() const;
	LibJitTypeTranslator& getTranslator();
	const LibJitTypeTranslator& getTranslator() const;
};
}

#endif // LIBJITCONTEXT_HPP
