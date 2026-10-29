#ifndef LIBJITCONTEXT_HPP
#define LIBJITCONTEXT_HPP
#include "LibJitTypeTranslation.hpp"

/*!
  \file LibJitContext.hpp
  \brief The compilation context and the type translator it owns.

  This header is internal: it is not part of the embedding surface.
*/

namespace LibJIT {
//! Owns the backend's compilation context and the one type translator that goes
//! with it. Neither is copyable: the translator caches handles that belong to the
//! context, so the two lifetimes are the same lifetime.
class Context
{
private:
	//! The backend's own context handle; destroyed with this object.
	jit_context_t context;
	//! Canonical-type to lowered-type translation, shared by everything compiled
	//! in this context.
	LibJitTypeTranslator typeTranslator;
public:
	//! Creates a fresh context and its translator.
	Context();
	//! Releases the context; every function built from it must already be gone.
	~Context();
	//! The raw context handle, for the backend's own entry points.
	jit_context_t rawContext() const;
	//! The translator owned by this context.
	LibJitTypeTranslator& getTranslator();
	//! The translator owned by this context, read-only.
	const LibJitTypeTranslator& getTranslator() const;
};
}

#endif // LIBJITCONTEXT_HPP
