#ifndef LIBJITCONTEXT_HPP
#define LIBJITCONTEXT_HPP
#include "LibJitTypeTranslation.hpp"

/*!
  \file LibJitContext.hpp
  \brief The compilation context and the type translator it owns.

  This header is internal: it is not part of the embedding surface.
*/

namespace LibJIT {
/*!
 * \brief Owns a LibJIT compilation context and its associated type translator.
 *
 * The translator caches backend handles that are valid only for the lifetime
 * of the context. Consequently, the context and translator are deliberately
 * owned together and must not be copied.
 */
class Context
{
private:
	//! Backend compilation context; released when this object is destroyed.
	jit_context_t context;
	//! Canonical-Wasm-type to LibJIT-type translation state for this context.
	LibJitTypeTranslator typeTranslator;
public:
	/*!
	 * \brief Creates a backend context and its type translator.
	 *
	 * \post rawContext() refers to the newly created context.
	 */
	Context();
	/*!
	 * \brief Releases the backend context.
	 *
	 * All functions and other backend objects created from this context must
	 * have been released before the Context is destroyed.
	 */
	~Context();
	/*!
	 * \brief Returns the underlying LibJIT context handle.
	 * \return The non-owning backend context handle.
	 */
	jit_context_t rawContext() const;
	/*!
	 * \brief Provides mutable access to this context's type translator.
	 * \return A reference to the translator owned by this Context.
	 */
	LibJitTypeTranslator& getTranslator();
	/*!
	 * \brief Provides read-only access to this context's type translator.
	 * \return A const reference to the translator owned by this Context.
	 */
	const LibJitTypeTranslator& getTranslator() const;
};
}

#endif // LIBJITCONTEXT_HPP
