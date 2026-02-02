#ifndef ELVCONTINUOUSITERATOR_HPP
#define ELVCONTINUOUSITERATOR_HPP
#include <cstddef>
#include <iterator>
namespace Elv {
namespace Util {

/**
 * \brief Template struct representing a continuous iterator.
 *
 * This iterator provides random access capabilities and is designed for traversing contiguous memory blocks.
 *
 * \tparam T The type of the elements being iterated over.
 */
template <typename T>
struct continuous_iterator {
public:
	/**
	 * \brief Iterator category tag indicating random access capability.
	 */
	using iterator_category = std::random_access_iterator_tag;

	/**
	 * \brief Signed type representing the difference between two iterators.
	 */
	using difference_type = ptrdiff_t;

	/**
	 * \brief Type of the elements being iterated over.
	 */
	using value_type = T;

	/**
	 * \brief Pointer type to the elements being iterated over.
	 */
	using pointer = T*;

	/**
	 * \brief Reference type to the elements being iterated over.
	 */
	using reference = T&;

private:
	/**
	 * \brief Internal pointer to the current element.
	 */
	pointer m_ptr;

public:
	/**
	 * \brief Constructor initializing the iterator with a pointer.
	 *
	 * \param ptr Pointer to the starting element.
	 */
	continuous_iterator(pointer ptr) : m_ptr(ptr) {}

	/**
	 * \brief Dereference operator returning a reference to the current element.
	 *
	 * \return Reference to the current element.
	 */
	reference operator*() const { return *m_ptr; }

	/**
	 * \brief Member access operator returning a pointer to the current element.
	 *
	 * \return Pointer to the current element.
	 */
	pointer operator->() { return m_ptr; }

	/**
	 * \brief Subscript operator for accessing elements at an offset.
	 *
	 * \param i Offset from the current element.
	 * \return Reference to the element at the specified offset.
	 */
	reference operator[](difference_type i) { return m_ptr[i]; }

	/**
	 * \brief Prefix increment operator moving to the next element.
	 *
	 * \return Reference to the incremented iterator.
	 */
	continuous_iterator& operator++() { ++m_ptr; return *this; }

	/**
	 * \brief Postfix increment operator moving to the next element.
	 *
	 * \param int (Unused, for syntax compliance)
	 * \return Copy of the iterator before increment.
	 */
	continuous_iterator operator++(int) { continuous_iterator tmp = *this; ++(*this); return tmp; }

	/**
	 * \brief Prefix decrement operator moving to the previous element.
	 *
	 * \return Reference to the decremented iterator.
	 */
	continuous_iterator& operator--() { --m_ptr; return *this; }

	/**
	 * \brief Postfix decrement operator moving to the previous element.
	 *
	 * \param int (Unused, for syntax compliance)
	 * \return Copy of the iterator before decrement.
	 */
	continuous_iterator operator--(int) { continuous_iterator tmp = *this; --(*this); return tmp; }

	/**
	 * \brief Addition operator creating an iterator to an element at a specified offset.
	 *
	 * \param i Offset from the current element.
	 * \return New iterator pointing to the element at the specified offset.
	 */
	continuous_iterator operator+(difference_type i) const { return continuous_iterator(m_ptr + i); }

	/**
	 * \brief Subtraction operator creating an iterator to an element at a specified negative offset.
	 *
	 * \param i Negative offset from the current element.
	 * \return New iterator pointing to the element at the specified negative offset.
	 */
	continuous_iterator operator-(difference_type i) const { return continuous_iterator(m_ptr - i); }

	/**
	 * \brief Difference operator calculating the distance between two iterators.
	 *
	 * \param other Iterator to calculate the distance from.
	 * \return Signed difference between the two iterators.
	 */
	difference_type operator-(const continuous_iterator& other) const { return m_ptr - other.m_ptr; }

	/**
	 * \brief Compound assignment operator for adding an offset to the current iterator.
	 *
	 * \param i Offset to add.
	 * \return Reference to the modified iterator.
	 */
	continuous_iterator& operator+=(difference_type i) { m_ptr += i; return *this; }

	/**
	 * \brief Compound assignment operator for subtracting an offset from the current iterator.
	 *
	 * \param i Offset to subtract.
	 * \return Reference to the modified iterator.
	 */
	continuous_iterator& operator-=(difference_type i) { m_ptr -= i; return *this; }

	/**
	 * \name Comparison Operators
	 * \{
	 */

	/**
	 * \brief Equality comparison operator.
	 *
	 * \param a First iterator for comparison.
	 * \param b Second iterator for comparison.
	 * \return True if both iterators point to the same element, false otherwise.
	 */
	friend bool operator==(const continuous_iterator& a, const continuous_iterator& b) { return a.m_ptr == b.m_ptr; }

	/**
	 * \brief Inequality comparison operator.
	 *
	 * \param a First iterator for comparison.
	 * \param b Second iterator for comparison.
	 * \return True if iterators do not point to the same element, false otherwise.
	 */
	friend bool operator!=(const continuous_iterator& a, const continuous_iterator& b) { return a.m_ptr!= b.m_ptr; }

	/**
	 * \brief Less-than comparison operator.
	 *
	 * \param a First iterator for comparison.
	 * \param b Second iterator for comparison.
	 * \return True if the first iterator points to an element before the second, false otherwise.
	 */
	friend bool operator<(const continuous_iterator& a, const continuous_iterator& b) { return a.m_ptr < b.m_ptr; }

	/**
	 * \brief Greater-than comparison operator.
	 *
	 * \param a First iterator for comparison.
	 * \param b Second iterator for comparison.
	 * \return True if the first iterator points to an element after the second, false otherwise.
	 */
	friend bool operator>(const continuous_iterator& a, const continuous_iterator& b) { return a.m_ptr > b.m_ptr; }

	/**
	 * \brief Less-than-or-equal comparison operator.
	 *
	 * \param a First iterator for comparison.
	 * \param b Second iterator for comparison.
	 * \return True if the first iterator points to an element before or at the same position as the second, false otherwise.
	 */
	friend bool operator<=(const continuous_iterator& a, const continuous_iterator& b) { return a.m_ptr <= b.m_ptr; }

	/**
	 * \brief Greater-than-or-equal comparison operator.
	 *
	 * \param a First iterator for comparison.
	 * \param b Second iterator for comparison.
	 * \return True if the first iterator points to an element after or at the same position as the second, false otherwise.
	 */
	friend bool operator>=(const continuous_iterator& a, const continuous_iterator& b) { return a.m_ptr >= b.m_ptr; }

	/** \} */

	/**
	 * \brief Returns the internal pointer for compatibility with contiguous iterators.
	 *
	 * \return Pointer to the current element.
	 */
	pointer get_pointer() const { return m_ptr; }

	/**
	 * \brief Implicit conversion operator to a raw pointer for std::contiguous_iterator concept compliance.
	 *
	 * \return Pointer to the current element.
	 */
	operator pointer() const { return m_ptr; }
};

}
}
#endif // ELVCONTINUOUSITERATOR_HPP
