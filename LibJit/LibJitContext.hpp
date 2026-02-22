#ifndef LIBJITCONTEXT_HPP
#define LIBJITCONTEXT_HPP
#include "LibJitTypeTranslation.hpp"

namespace LibJIT {
class Context
{
private:
	jit_context_t context;
	LibJitTypeTranslator typeTranslator;
public:
	Context();
	~Context();
	LibJitTypeTranslator& getTranslator();
	const LibJitTypeTranslator& getTranslator() const;
};
}

#endif // LIBJITCONTEXT_HPP
