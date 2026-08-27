#ifndef WASMSTORE_HPP
#define WASMSTORE_HPP
#include "WasmMemory.hpp"
#include <vector>
#include <unordered_map>
#include <atomic>
namespace WASM {
class Store
{
public:
	typedef std::unordered_map<int, std::unique_ptr<StoreOwnedMemory>> MemoryMap;
	typedef MemoryMap::iterator MemoryIterator;
private:
	MemoryMap memories;
	std::atomic<int> memMaxId;
	static std::unique_ptr<StoreOwnedMemory> _createLinearMemory__(uint64_t initialPages, uint64_t maxPages, bool isShared);
public:
	Store();
	MemoryIterator createLinearMemory(uint64_t initialPages, uint64_t maxPages, bool isShared);
};
}
#endif // WASMSTORE_HPP
