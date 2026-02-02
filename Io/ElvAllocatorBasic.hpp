#ifndef ELVALLOCATORBASIC_HPP
#define ELVALLOCATORBASIC_HPP
#include <cstddef>
#include <concepts>
#include <memory_resource>
#include <new>
#include <memory>
#include <functional>

/**
 * @defgroup MemoryManagement Memory Management Utilities
 * @{
 */
namespace Elv {
namespace Util {

/**
 * @concept Allocator
 * @brief Concept for an allocator type, ensuring it meets the necessary interface for memory management.
 *
 * @tparam Alloc Allocator type to check.
 * @tparam T Type of objects to be allocated.
 */
template <typename Alloc, typename T> concept Allocator = requires(Alloc alloc, T* ptr, std::size_t n, T t) {
	/// @brief Ensure Alloc has a value_type member.
	typename Alloc::value_type;
	/// @brief Require Alloc's value_type to be the same as T.
	requires std::same_as<typename Alloc::value_type, T>;
	/// @brief Verify alloc can allocate n elements of T, returning a T*.
	{ alloc.allocate(n) } -> std::same_as<T*>;
	/// @brief Ensure alloc can deallocate ptr with size n.
	alloc.deallocate(ptr, n);
	/// @brief Ensure Alloc can rebind to a different type U.
	//typename Alloc::template rebind<int>::other;
	/// @brief Verify the rebind result has a value_type of int.
	//requires std::same_as<typename Alloc::template rebind<int>::other::value_type, int>;
};

/**
 * @class SmartPointerWrappersForAlloc
 * @brief Provides smart pointer wrappers (unique_ptr, shared_ptr, weak_ptr) for a given type T and allocator Alloc.
 *
 * @tparam T Type of the objects to be managed.
 * @tparam Alloc Allocator type (defaults to std::allocator<T>).
 */
template <typename T, typename Alloc = std::allocator<T>> requires Allocator<Alloc, T> struct SmartPointerWrappersForAlloc {
public:
	/**
	 * @struct Deleter
	 * @brief Custom deleter for smart pointers, handling object destruction and memory deallocation.
	 */
	struct Deleter {
		Alloc* allocator; ///< Allocator instance (or nullptr for default-constructed temporary allocator).
		/**
		 * @brief Calls destruction on the object and then deallocates the memory using the provided allocator.
		 * @param ptr Pointer to the object to delete.
		 */
		void operator()(T* ptr) const {
			if (allocator) {
				std::destroy_at(ptr);
				allocator->deallocate(ptr, 1);
			} else {
				Alloc tempAlloc;
				std::destroy_at(ptr);
				tempAlloc.deallocate(ptr, 1);
			}
		}
	};

private:
	/**
	 * @brief Creates a new object of type T using the default allocator.
	 * @tparam Args Parameter pack for T's constructor.
	 * @param args Forwarded arguments for T's constructor.
	 * @return Pointer to the newly created T object.
	 */
	template <typename... Args> static T* create(Args&&... args) {
		Alloc allocator;
		T* toReturn = allocator.allocate(1);
		std::construct_at(toReturn, std::forward(args)...);
		return toReturn;
	}

	/**
	 * @brief Creates a new object of type T using a provided allocator.
	 * @tparam Args Parameter pack for T's constructor.
	 * @param allocator Allocator instance to use.
	 * @param args Forwarded arguments for T's constructor.
	 * @return Pointer to the newly created T object.
	 */
	template <typename... Args> static T* createWithAllocator(Alloc& allocator, Args&&... args) {
		T* toReturn = allocator.allocate(1);
		std::construct_at(toReturn, std::forward(args)...);
		return toReturn;
	}

public:
	/// @brief Unique pointer type with custom Deleter.
	typedef std::unique_ptr<T, Deleter> unique_ptr;
	/// @brief Shared pointer type.
	typedef std::shared_ptr<T> shared_ptr;
	/// @brief Weak pointer type.
	typedef std::weak_ptr<T> weak_ptr;

	/**
	 * @brief Creates a unique_ptr to a new T object, using the provided allocator.
	 * @tparam Args Parameter pack for T's constructor.
	 * @param allocator Allocator instance to use.
	 * @param args Forwarded arguments for T's constructor.
	 * @return unique_ptr to the newly created T object.
	 */
	template <typename... Args> static unique_ptr make_unique(Alloc& allocator, Args&&... args) {
		return unique_ptr(createWithAllocator(allocator, std::forward(args)...), Deleter{ &allocator });
	}

	/**
	 * @brief Creates a unique_ptr to a new T object, using a temporary default allocator.
	 * @tparam Args Parameter pack for T's constructor.
	 * @param args Forwarded arguments for T's constructor.
	 * @return unique_ptr to the newly created T object.
	 */
	template <typename... Args> static unique_ptr make_unique(Args&&... args) {
		return unique_ptr(create(std::forward(args)...), Deleter { nullptr });
	}

	/**
	 * @brief Creates a shared_ptr to a new T object, using the provided allocator.
	 * @tparam Args Parameter pack for T's constructor.
	 * @param allocator Allocator instance to use.
	 * @param args Forwarded arguments for T's constructor.
	 * @return shared_ptr to the newly created T object.
	 */
	template <typename... Args> static shared_ptr make_shared(Alloc& allocator, Args&&... args) {
		return shared_ptr(createWithAllocator(allocator, std::forward(args)...), Deleter{ &allocator });
	}

	/**
	 * @brief Creates a shared_ptr to a new T object, using a temporary default allocator.
	 * @tparam Args Parameter pack for T's constructor.
	 * @param args Forwarded arguments for T's constructor.
	 * @return shared_ptr to the newly created T object.
	 */
	template <typename... Args> static shared_ptr make_shared(Args&&... args) {
		return shared_ptr(create(std::forward(args)...), Deleter { nullptr });
	}
};

/**
 * @overload make_unique
 * @brief Free function variant of make_unique, mirroring the member function of the same name.
 */
template <typename T, typename Alloc = std::allocator<T>, typename... Args> requires Allocator<Alloc, T>
SmartPointerWrappersForAlloc<T, Alloc>::unique_ptr make_unique(Alloc & allocator, Args&&... args) {
	return SmartPointerWrappersForAlloc<T, Alloc>::make_unique(allocator, std::forward(args)...);
}

/**
 * @overload make_unique
 * @brief Free function variant of make_unique, using a temporary default allocator.
 */
template <typename T, typename Alloc = std::allocator<T>, typename... Args> requires Allocator<Alloc, T>
SmartPointerWrappersForAlloc<T, Alloc>::unique_ptr make_unique(Args&&... args) {
	return SmartPointerWrappersForAlloc<T, Alloc>::make_unique(std::forward(args)...);
}

/**
 * @overload make_shared
 * @brief Free function variant of make_shared, mirroring the member function of the same name.
 */
template <typename T, typename Alloc = std::allocator<T>, typename... Args> requires Allocator<Alloc, T>
SmartPointerWrappersForAlloc<T, Alloc>::shared_ptr make_shared(Alloc & allocator, Args&&... args) {
	return SmartPointerWrappersForAlloc<T, Alloc>::make_shared(allocator, std::forward(args)...);
}

/**
 * @overload make_shared
 * @brief Free function variant of make_shared, using a temporary default allocator.
 */
template <typename T, typename Alloc = std::allocator<T>, typename... Args> requires Allocator<Alloc, T>
SmartPointerWrappersForAlloc<T, Alloc>::shared_ptr make_shared(Args&&... args) {
	return SmartPointerWrappersForAlloc<T, Alloc>::make_shared(std::forward(args)...);
}

typedef std::function<void(void*)> GenericDeleter;
template <typename T> struct PolyMorphicSmartPointerFactory {
public:
	// Typedefs for the smart pointers using polymorphic_allocator
	using unique_ptr_type = std::unique_ptr<T, GenericDeleter>;
	using shared_ptr_type = std::shared_ptr<T>;

	// Static function to create a unique_ptr<T>
	template <typename... Args> static unique_ptr_type make_unique(std::pmr::memory_resource* resource = std::pmr::get_default_resource(), Args&&... args) {
		std::pmr::polymorphic_allocator<T> alloc(resource);
		T* ptr = alloc.allocate(1);
		try {
			alloc.construct(ptr, std::forward<Args>(args)...);
		} catch (...) {
			alloc.deallocate(ptr, 1);
			throw;
		}
		auto deleter = [resource](void* ptr) {
			if(ptr) {
				static_cast<T*>(ptr)->~T();
				std::pmr::polymorphic_allocator<T> alloc(resource);
				alloc.deallocate(static_cast<T*>(ptr), 1);
			}
		};
		return unique_ptr_type(ptr, deleter );
	}

	// Static function to create a shared_ptr<T>
	template <typename... Args> static shared_ptr_type make_shared(std::pmr::memory_resource* resource = std::pmr::get_default_resource(), Args&&... args) {
		std::pmr::polymorphic_allocator<T> alloc(resource);
		T* ptr = alloc.allocate(1);
		try {
			alloc.construct(ptr, std::forward<Args>(args)...);
		} catch (...) {
			alloc.deallocate(ptr, 1);
			throw;
		}
		auto deleter = [resource](void* ptr) {
			if(ptr) {
				static_cast<T*>(ptr)->~T();
				std::pmr::polymorphic_allocator<T> alloc(resource);
				alloc.deallocate(static_cast<T*>(ptr), 1);
			}
		};
		return shared_ptr_type(ptr, deleter, alloc);
	}
};
template <typename T, typename... Args>
PolyMorphicSmartPointerFactory<T>::unique_ptr_type pmr_make_unique(std::pmr::memory_resource* resource = std::pmr::get_default_resource(), Args&&... args) {
	return PolyMorphicSmartPointerFactory<T>::make_unique(resource, std::forward<Args>(args)... );
}
template <typename T, typename... Args>
PolyMorphicSmartPointerFactory<T>::shared_ptr_type pmr_make_shared(std::pmr::memory_resource* resource = std::pmr::get_default_resource(), Args&&... args) {
	return PolyMorphicSmartPointerFactory<T>::make_shared(resource, std::forward<Args>(args)... );
}



/**
 * @struct Blk
 * @brief Structure representing a memory block.
 */
struct Blk {
	void* ptr; ///< Pointer to the memory block.
	size_t size; ///< Size of the memory block.
};

/**
 * @concept AlexandrescuAllocator
 * @brief Concept for an allocator type following the Alexandrescu style, providing block-level memory management.
 *
 * @tparam Alloc Allocator type to check.
 */
template <typename Alloc> concept AlexandrescuAllocator = requires(Alloc alloc, std::size_t n, Blk blk, const Blk& blkcref) {
	/// @brief Ensure alloc can allocate a block of size n, returning a Blk.
	{ alloc.allocateBlock(n) } -> std::same_as<Blk>;
	/// @brief Verify alloc can deallocate a block.
	{ alloc.deallocateBlock(blkcref) };
	/// @brief Require alloc to check ownership of a block, returning a bool.
	{ alloc.ownsBlock(blkcref) } -> std::same_as<bool>;
};

/**
 * @class AlexandrescuAllocatorAdapter
 * @brief Adapts an Alexandrescu-style allocator to the Standard Library's allocator interface.
 *
 * @tparam Alloc Alexandrescu-style allocator type.
 * @tparam T Type of objects to be allocated.
 */
template <typename Alloc, typename T> requires AlexandrescuAllocator<Alloc> struct AlexandrescuAllocatorAdapter {
	/// @brief Reference type.
	typedef T& reference;
	/// @brief Const reference type.
	typedef const T& const_reference;
	/// @brief Pointer type.
	typedef T* pointer;
	/// @brief Const pointer type.
	typedef const T* const_pointer;
	/// @brief Void pointer type.
	typedef void* void_pointer;
	/// @brief Const void pointer type.
	typedef const void* const_void_pointer;
	/// @brief Value type.
	typedef T value_type;
	/// @brief Size type.
	typedef std::size_t size_type;
	/// @brief Difference type.
	typedef std::ptrdiff_t difference_type;
	/// @brief Allocator type itself.
	typedef AlexandrescuAllocatorAdapter allocator_type;
	/// @brief Propagation trait for container copy assignment.
	typedef std::false_type propagate_on_container_copy_assignment;
	/// @brief Propagation trait for container move assignment.
	typedef std::false_type propagate_on_container_move_assignment;
	/// @brief Propagation trait for container swap.
	typedef std::false_type propagate_on_container_swap;
	/// @brief Equality trait (always equal for this adapter).
	typedef std::true_type is_always_equal;

	/// @brief Underlying Alexandrescu-style allocator instance.
	Alloc alloc_;

	/**
	 * @brief Default constructor.
	 */
	AlexandrescuAllocatorAdapter() = default;
	AlexandrescuAllocatorAdapter(const Alloc& cpy) : alloc_(cpy) {

	}
	AlexandrescuAllocatorAdapter(Alloc&& mov) : alloc_(std::move(mov)) {

	}
	template<typename U> AlexandrescuAllocatorAdapter(const AlexandrescuAllocatorAdapter<Alloc,U>& cpy) : alloc_(cpy.alloc_) {

	}
	template<typename U> AlexandrescuAllocatorAdapter(AlexandrescuAllocatorAdapter<Alloc,U>&& mov) : alloc_(std::move(mov.alloc_)) {

	}
	template<typename U> AlexandrescuAllocatorAdapter& operator=(const AlexandrescuAllocatorAdapter<Alloc,U>& cpy) {
		this->alloc_ = cpy.alloc_;
		return *this;
	}
	template<typename U> AlexandrescuAllocatorAdapter& operator=(AlexandrescuAllocatorAdapter<Alloc,U>&& mov) {
		this->alloc_ = std::move(mov.alloc_);
		return *this;
	}
	template <typename... Args> AlexandrescuAllocatorAdapter(Args&&... args)
		: alloc_(std::forward(args)...)
	{

	}

	/**
	 * @brief Allocates memory for n objects of type T.
	 * @param n Number of objects to allocate memory for.
	 * @return Pointer to the beginning of the allocated memory.
	 * @throws std::bad_alloc if allocation fails.
	 */
	T* allocate(std::size_t n) {
		std::size_t total_size = n * sizeof(T);
		Blk blk = alloc_.allocateBlock(total_size);
		if (!blk.ptr) throw std::bad_alloc();
		return static_cast<T*>(blk.ptr);
	}

	/**
	 * @brief Deallocates memory previously allocated for n objects of type T.
	 * @param ptr Pointer to the memory to deallocate.
	 * @param n Number of objects the memory was allocated for.
	 */
	void deallocate(T* ptr, std::size_t n) {
		Blk blk{ static_cast<void*>(ptr), n * sizeof(T) };
		alloc_.deallocateBlock(blk);
	}

	/**
	 * @struct rebind
	 * @brief Helper for rebinding the allocator to a different type U.
	 *
	 * @tparam U New type to rebind the allocator to.
	 */
	template <typename U>
	struct rebind {
		/// @brief The rebound allocator type.
		using other = AlexandrescuAllocatorAdapter<Alloc, U>;
	};

	/**
	 * @brief Constructs an allocator from another AlexandrescuAllocatorAdapter instance.
	 * @tparam UAlloc Allocator type of the other instance.
	 * @tparam UT Type associated with the other instance.
	 * @param other Other AlexandrescuAllocatorAdapter instance.
	 */
	template <typename UAlloc, typename UT> requires AlexandrescuAllocator<UAlloc> constexpr AlexandrescuAllocatorAdapter(const AlexandrescuAllocatorAdapter <UAlloc, UT>&) noexcept {}

	/**
	 * @brief Equality operator (always returns true for this adapter).
	 * @param lhs Left-hand side allocator.
	 * @param rhs Right-hand side allocator.
	 * @return True.
	 */
	friend bool operator==(const AlexandrescuAllocatorAdapter& lhs, const AlexandrescuAllocatorAdapter& rhs) {
		return true;
	}

	/**
	 * @brief Inequality operator (always returns false for this adapter).
	 * @param lhs Left-hand side allocator.
	 * @param rhs Right-hand side allocator.
	 * @return False.
	 */
	friend bool operator!=(const AlexandrescuAllocatorAdapter& lhs, const AlexandrescuAllocatorAdapter& rhs) {
		return false;
	}
};

template <typename Alloc, typename T> requires AlexandrescuAllocator<Alloc> struct StaticAlexandrescuAllocatorAdapter {
	typedef T& reference;
	typedef const T& const_reference;
	typedef T* pointer;
	typedef const T* const_pointer;
	typedef void* void_pointer;
	typedef const void* const_void_pointer;
	typedef T value_type;
	typedef std::size_t size_type;
	typedef std::ptrdiff_t difference_type;
	typedef StaticAlexandrescuAllocatorAdapter allocator_type;
	typedef std::false_type propagate_on_container_copy_assignment;
	typedef std::false_type propagate_on_container_move_assignment;
	typedef std::false_type propagate_on_container_swap;
	typedef std::true_type is_always_equal;
	//! The underlying AlexandrescuAllocator
	static Alloc alloc_;

	//! Constructor
	StaticAlexandrescuAllocatorAdapter() = default;

	//! Allocate function. Returns a pointer to the allocated data.
	/*!
	\param n The amount of objects to allocate memory for. Size is given in object-count, NOT bytes!
	\return A pointer to the memory allocated.
	*/
	T* allocate(std::size_t n) {
		std::size_t total_size = n * sizeof(T);  // Calculate total size needed
		Blk blk = alloc_.allocateBlock(total_size);
		if (!blk.ptr) throw std::bad_alloc();
		return static_cast<T*>(blk.ptr);  // Return the pointer cast to T*
	}

	//! Deallocates memory.
	/*!
	\param ptr A pointer to the memory to deallocate.
	\param n The amount of objects to deallocate. Size is given in object-count, NOT bytes!
	*/
	void deallocate(T* ptr, std::size_t n) {
		Blk blk{ static_cast<void*>(ptr), n * sizeof(T) };
		alloc_.deallocateBlock(blk);
	}
	//! For STL compatibility
	template <typename U>
	struct rebind {
		using other = StaticAlexandrescuAllocatorAdapter<Alloc, U>;
	};
	//! For STL compatibility
	template <typename UAlloc, typename UT> requires AlexandrescuAllocator<UAlloc> constexpr StaticAlexandrescuAllocatorAdapter(const StaticAlexandrescuAllocatorAdapter <UAlloc, UT>&) noexcept {}
	friend bool operator==(const StaticAlexandrescuAllocatorAdapter& lhs, const StaticAlexandrescuAllocatorAdapter& rhs) {
		return true;
	}
	//! For STL compatibility
	friend bool operator!=(const StaticAlexandrescuAllocatorAdapter& lhs, const StaticAlexandrescuAllocatorAdapter& rhs) {
		return false;
	}
};

/**
 * @class AlexandrescuMemoryResource
 * @brief A memory resource that uses an allocator following the Alexandrescu style.
 *
 * This class implements the `std::pmr::memory_resource` interface, using an allocator that adheres to the
 * Alexandrescu style for block-level memory management. The allocator must satisfy the `AlexandrescuAllocator`
 * concept, providing methods to allocate, deallocate, and check ownership of memory blocks.
 *
 * @tparam Alloc The allocator type to be used, which must meet the `AlexandrescuAllocator` concept.
 */
template <AlexandrescuAllocator Alloc>
class AlexandrescuMemoryResource : public std::pmr::memory_resource {
private:
	Alloc allocator_; ///< The allocator instance used for memory management.

public:
	/**
	 * @brief Default constructor.
	 */
	AlexandrescuMemoryResource() = default;

	/**
	 * @brief Copy constructor.
	 *
	 * @param cpy The allocator instance to copy.
	 */
	AlexandrescuMemoryResource(const Alloc& cpy) : allocator_(cpy) {}

	/**
	 * @brief Move constructor.
	 *
	 * @param mov The allocator instance to move.
	 */
	AlexandrescuMemoryResource(Alloc&& mov) : allocator_(std::move(mov)) {}

	/**
	 * @brief Constructor with variadic arguments.
	 *
	 * @tparam Args Types of the arguments to forward to the allocator's constructor.
	 * @param args Arguments to forward to the allocator's constructor.
	 */
	template <typename... Args>
	AlexandrescuMemoryResource(Args&&... args) : allocator_(std::forward<Args>(args)...) {}

protected:
	/**
	 * @brief Allocates a block of memory.
	 *
	 * This method overrides the `do_allocate` method from `std::pmr::memory_resource` to allocate a block
	 * of memory using the provided allocator.
	 *
	 * @param bytes The size of the memory block to allocate.
	 * @param alignment The alignment requirement for the memory block.
	 * @return A pointer to the allocated memory block, or `nullptr` if allocation fails.
	 */
	void* do_allocate(std::size_t bytes, std::size_t alignment) override {
		// Allocate a block of memory using the allocator
		Blk blk = allocator_.allocateBlock(bytes);
		// If the block is invalid, return nullptr
		if (blk.ptr == nullptr) {
			return nullptr;
		}
		// Return the pointer of the allocated block
		return blk.ptr;
	}

	/**
	 * @brief Deallocates a block of memory.
	 *
	 * This method overrides the `do_deallocate` method from `std::pmr::memory_resource` to deallocate a block
	 * of memory using the provided allocator.
	 *
	 * @param p Pointer to the memory block to deallocate.
	 * @param bytes The size of the memory block to deallocate.
	 * @param alignment The alignment requirement for the memory block.
	 */
	void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
		// We need to pass a Blk to deallocate, so we'll assume the memory resource
		// is using the pointer to locate the corresponding block.
		Blk blk{p, bytes};
		allocator_.deallocateBlock(blk);
	}

	/**
	 * @brief Checks if two memory resources are equal.
	 *
	 * This method overrides the `do_is_equal` method from `std::pmr::memory_resource` to compare two memory
	 * resources. Two resources are considered equal if they wrap the same allocator instance.
	 *
	 * @param other The memory resource to compare with.
	 * @return `true` if the resources are equal, `false` otherwise.
	 */
	bool do_is_equal(const memory_resource& other) const noexcept override {
		// This could be refined based on your allocator's requirements
		// In this example, we consider two memory resources equal if they wrap the same allocator.
		auto& other_resource = static_cast<const AlexandrescuMemoryResource&>(other);
		return &allocator_ == &other_resource.allocator_;
	}
};

/**
 * @class FallbackAllocator
 * @brief Allocator that falls back to a secondary allocator if the primary one fails to allocate or deallocate.
 *
 * @tparam Primary Primary allocator type.
 * @tparam Fallback Secondary allocator type.
 */
template <typename Primary, typename Fallback> requires AlexandrescuAllocator<Primary> && AlexandrescuAllocator<Fallback>
struct FallbackAllocator : private Primary, private Fallback {
	/**
	 * @brief Allocates a block of memory, falling back to Fallback if Primary fails.
	 * @param n Size of the block to allocate.
	 * @return Allocated block.
	 */
	Blk allocateBlock(std::size_t n) {
		Blk r = Primary::allocateBlock(n);
		if(!r.ptr) r = Fallback::allocateBlock(n);
		return r;
	}

	/**
	 * @brief Deallocates a block, trying Primary first, then Fallback.
	 * @param blk Block to deallocate.
	 */
	void deallocateBlock(const Blk& blk) {
		if(Primary::ownsBlock(blk)) Primary::deallocateBlock(blk);
		else Fallback::deallocateBlock(blk);
	}

	/**
	 * @brief Checks ownership of a block, considering both Primary and Fallback.
	 * @param blk Block to check.
	 * @return True if either Primary or Fallback owns the block.
	 */
	bool ownsBlock(const Blk& blk) {
		return Primary::ownsBlock(blk) || Fallback::ownsBlock(blk);
	}
};

/**
 * @class SegregatorAllocator
 * @brief Allocator that segregates allocations based on size, using SmallAllocator for smaller allocations and LargeAllocator for larger ones.
 *
 * @tparam threshold Size threshold for allocation segregation.
 * @tparam SmallAllocator Allocator for smaller allocations.
 * @tparam LargeAllocator Allocator for larger allocations.
 */
template <size_t threshold, typename SmallAllocator, typename LargeAllocator> requires AlexandrescuAllocator<SmallAllocator> && AlexandrescuAllocator<LargeAllocator>
struct SegregatorAllocator : private SmallAllocator, private LargeAllocator {
	/**
	 * @brief Allocates a block of memory, choosing the allocator based on the size.
	 * @param n Size of the block to allocate.
	 * @return Allocated block.
	 */
	Blk allocateBlock(std::size_t n) {
		if(n <= threshold) return SmallAllocator::allocateBlock(n);
		else return LargeAllocator::allocateBlock(n);
	}

	/**
	 * @brief Deallocates a block, selecting the allocator based on the block's size.
	 * @param blk Block to deallocate.
	 */
	void deallocateBlock(const Blk& blk) {
		if(blk.size <= threshold) return SmallAllocator::deallocateBlock(blk);
		else return LargeAllocator::deallocateBlock(blk);
	}

	/**
	 * @brief Checks ownership of a block, considering both allocators.
	 * @param blk Block to check.
	 * @return True if either allocator owns the block.
	 */
	bool ownsBlock(const Blk& blk) {
		return SmallAllocator::ownsBlock(blk) || LargeAllocator::ownsBlock(blk);
	}
};

}
}

/**
 * @def DEFINE_CLASS_WITH_POLYMORPHIC_ALLOCATOR
 * @brief Macro to define smart pointer types (unique, shared, weak) for a class with a polymorphic allocator.
 *
 * @param Klass Class name.
 */
#define DEFINE_CLASS_WITH_POLYMORPHIC_ALLOCATOR(Klass) class Klass; \
typedef Elv::Util::PolyMorphicSmartPointerFactory<Klass> Klass##_PolymorphicPointerFactory; \
	typedef Klass##_PolymorphicPointerFactory::unique_ptr_type u##Klass; \
	typedef Klass##_PolymorphicPointerFactory::shared_ptr_type s##Klass; \
	typedef std::weak_ptr<Klass> w##Klass;


/**
 * @def DEFINE_STRUCT_WITH_POLYMORPHIC_ALLOCATOR
 * @brief Macro to define smart pointer types (unique, shared, weak) for a struct with a polymorphic allocator.
 *
 * @param Klass Struct name.
 */
#define DEFINE_STRUCT_WITH_POLYMORPHIC_ALLOCATOR(Klass) struct Klass; \
typedef Elv::Util::PolyMorphicSmartPointerFactory<Klass> Klass##_PolymorphicPointerFactory; \
	typedef Klass##_PolymorphicPointerFactory::unique_ptr_type u##Klass; \
	typedef Klass##_PolymorphicPointerFactory::shared_ptr_type s##Klass; \
	typedef std::weak_ptr<Klass> w##Klass;

/**
 * @def DEFINE_STRUCT_PTRS_WITH_ALLOC
 * @brief Macro to define smart pointer types (unique, shared, weak) for a struct with a custom allocator.
 *
 * @param Klass Struct name.
 * @param Alloc Allocator type.
 */
#define DEFINE_STRUCT_PTRS_WITH_ALLOC(Klass,Alloc) struct Klass; \
	typedef Elv::Util::SmartPointerWrappersForAlloc<Klass,Alloc> Klass##_Alloc; \
	typedef Klass##_Alloc::unique_ptr u##Klass; \
	typedef Klass##_Alloc::shared_ptr s##Klass; \
	typedef Klass##_Alloc::weak_ptr w##Klass;

/**
 * @def DEFINE_CLASS_PTRS_WITH_ALLOC
 * @brief Macro to define smart pointer types (unique, shared, weak) for a class with a custom allocator.
 *
 * @param Klass Class name.
 * @param Alloc Allocator type.
 */
#define DEFINE_CLASS_PTRS_WITH_ALLOC(Klass,Alloc) class Klass; \
	typedef Elv::Util::SmartPointerWrappersForAlloc<Klass,Alloc> Klass##_Alloc; \
	typedef Klass##_Alloc::unique_ptr u##Klass; \
	typedef Klass##_Alloc::shared_ptr s##Klass; \
	typedef Klass##_Alloc::weak_ptr w##Klass;

/// @} // End of MemoryManagement group
#endif // ELVALLOCATORBASIC_HPP
