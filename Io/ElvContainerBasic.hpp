#ifndef ELVCONTAINERBASIC_HPP
#define ELVCONTAINERBASIC_HPP
#include <concepts>
#include <iterator>
#include <type_traits>
#include <utility>
namespace Elv {
namespace Util {

/**
 * @brief Concept defining common properties and operations of standard-like containers.
 *
 * @tparam T The container type being constrained.
 */
template<typename T>
concept ContainerCommon = requires(T container, const T const_container) {
	/// @name Type Definitions
	/// @{
	typename T::value_type;       ///< The type of elements stored in the container.
	typename T::reference;        ///< Reference type to elements in the container.
	typename T::const_reference;  ///< Const reference type to elements in the container.
	typename T::iterator;         ///< Iterator type for the container.
	typename T::const_iterator;   ///< Const iterator type for the container.
	typename T::difference_type;  ///< Difference type used for indexing.
	typename T::size_type;        ///< Size type for container sizes.
	/// @}

	/// @name Common Operations
	/// @{
	{ container.begin() } -> std::same_as<typename T::iterator>;           ///< Returns an iterator to the beginning.
	{ container.end() } -> std::same_as<typename T::iterator>;             ///< Returns an iterator to the end.
	{ const_container.begin() } -> std::same_as<typename T::const_iterator>; ///< Returns a const iterator to the beginning.
	{ const_container.end() } -> std::same_as<typename T::const_iterator>;   ///< Returns a const iterator to the end.
	{ container.empty() } -> std::same_as<bool>;                           ///< Checks if the container is empty.
	{ container.size() } -> std::same_as<typename T::size_type>;           ///< Returns the number of elements.
	{ container.max_size() } -> std::same_as<typename T::size_type>;       ///< Returns the maximum possible number of elements.
	{ container.clear() } -> std::same_as<void>;                           ///< Clears the container.
	/// @}
};

/**
 * @brief Concept for vector-like containers, such as std::vector.
 *
 * @tparam T The container type being constrained.
 */
template<typename T>
concept VectorLike = ContainerCommon<T> && requires(T container,
													const typename T::value_type& value,
													typename T::size_type pos,
													typename T::size_type count) {

	/// @name Vector-Specific Operations
	/// @{
	{ container.push_back(value) } -> std::same_as<void>; ///< Appends an element to the end.
	{ container.pop_back() } -> std::same_as<void>;       ///< Removes the last element.
	{ container[pos] } -> std::same_as<typename T::reference>; ///< Accesses an element by index.
	{ container.data() } -> std::same_as<typename T::value_type*>; ///< Returns a pointer to the underlying data.
	{ container.reserve(count) } -> std::same_as<void>; ///< Reserves storage for at least `count` elements.
	{ container.resize(count) } -> std::same_as<void>;  ///< Resizes the container to contain `count` elements.
	{ container.capacity() } -> std::same_as<typename T::size_type>; ///< Returns the number of elements that can be held before reallocation.
	/// @}

	/// @name Random Access Requirement
	/// @{
	requires std::random_access_iterator<typename T::iterator>; ///< Ensures random-access iterators.
	/// @}
};

/**
 * @brief Concept for map-like containers, such as std::map or std::unordered_map.
 *
 * @tparam T The container type being constrained.
 */
template<typename T>
concept MapLike = ContainerCommon<T> && requires(T container,
												 const typename T::key_type& key,
												 const typename T::mapped_type& value) {

	/// @name Type Definitions
	/// @{
	typename T::key_type;   ///< Type of the keys in the container.
	typename T::mapped_type; ///< Type of the values in the container.
	requires std::same_as<typename T::value_type, std::pair<const typename T::key_type, typename T::mapped_type>>;
	/// @}

	/// @name Map-Specific Operations
	/// @{
	{ container[key] } -> std::same_as<typename T::mapped_type&>; ///< Accesses an element by key.
	{ container.find(key) } -> std::same_as<typename T::iterator>; ///< Finds an element by key.
	{ container.count(key) } -> std::same_as<typename T::size_type>; ///< Counts occurrences of a key.
	{ container.contains(key) } -> std::same_as<bool>; ///< Checks if the key exists.
	{ container.insert({key, value}) } -> std::same_as<std::pair<typename T::iterator, bool>>; ///< Inserts a key-value pair.
	{ container.erase(key) } -> std::same_as<typename T::size_type>; ///< Erases an element by key.
	/// @}

	/// @name Iterator Compatibility
	/// @{
	requires std::same_as<typename std::iterator_traits<typename T::iterator>::value_type, typename T::value_type>; ///< Ensures iterator's value_type matches container's value_type.
	/// @}
};

/**
 * @brief Concept for sequential containers that are neither vector-like nor map-like.
 *
 * This includes linked lists, deques, and other sequential containers.
 *
 * @tparam T The container type being constrained.
 */
template<typename T>
concept SequentialContainer = ContainerCommon<T> &&
							  !VectorLike<T> &&
							  !MapLike<T> &&
							  requires(T container, const typename T::value_type& value) {

								  /// @name Sequential Container-Specific Operations
								  /// @{
								  { container.insert(container.begin(), value) } -> std::same_as<typename T::iterator>; ///< Inserts an element at a specific position.
								  { container.erase(container.begin()) } -> std::same_as<typename T::iterator>; ///< Erases an element at a specific position.
								  /// @}
							  };


}
}

#endif // ELVCONTAINERBASIC_HPP
