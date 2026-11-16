#ifndef WASISTUB_HPP
#define WASISTUB_HPP
#include <WasmBase/WasmComponentValue.hpp>
#include <WasmBase/WasmMemory.hpp>
#include <WasmBase/WasmNativeImport.hpp>
#include <WasmBase/WasmRegistryImportResolver.hpp>
#include <WasmBase/WasmTrap.hpp>
#include <WasmBase/WasmVMContext.hpp>
#include <WasmBase/WasmValue.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

/*!
  \file WasiStub.hpp
  \brief A hermetic, deliberately fake WASI 0.1 surface for test fixtures.

  This is **not** a WASI implementation and must never become one. It exists so a
  fixture that imports `wasi_snapshot_preview1` can be instantiated and called in
  the suite without a file, socket, wall clock or entropy source being touched:

  - `fd_write` appends to a sink owned by the fixture; nothing reaches a console;
  - `fd_read` reports end of file, and every `path_*` call is refused with
    `ENOSYS`, so a fixture cannot reach the host filesystem;
  - `clock_time_get` reports a fixed tick count and `random_get` fills a fixed
    pattern; the environment is empty and the argument list is `test`;
  - `proc_exit` records the code and raises an internal trap.

  The component side of the same fake lives in `WasiComponentHost.hpp`, which
  covers `wasi:io`, `wasi:cli`, `wasi:clocks`, `wasi:filesystem` and
  `wasi:random` — including resource types, whose handles a host bag owns.

  This header is internal: it is not part of the embedding surface.
*/

namespace WasiStub {

//! Everything the fake WASI has observed, so a fixture can assert on it.
struct State {
	std::string stdoutBytes; //!< Bytes written to `fd 1`.
	std::string stderrBytes; //!< Bytes written to `fd 2`.
	std::uint32_t fdWriteCalls = 0; //!< `fd_write` call count.
	std::uint32_t randomCalls = 0; //!< `random_get` call count.
	std::uint32_t clockCalls = 0; //!< `clock_time_get` call count.
	std::uint32_t refusedCalls = 0; //!< Calls refused with `ENOSYS`.
	//! Number of `wasi:io/streams` writes the component host captured.
	std::uint32_t streamWrites = 0;
	//! Bytes `wasi:io/streams` handed to the component host.
	std::string streamBytes;
	bool exitCalled = false; //!< True once `proc_exit` ran.
	std::int32_t exitCode = 0; //!< The code `proc_exit` received.
	std::uint64_t clockTicks = 4096; //!< The fixed tick count the clocks report.
};

/**
 * \brief Returns the process-wide fixture state.
 * \return The mutable state every fake entry point records into.
 */
inline State& state()
{
	static State instance;
	return instance;
}

/** \brief Clears the fixture state between cases. */
inline void reset()
{
	state() = State{};
}

//! `wasi_snapshot_preview1` errno values the stub uses.
enum : std::int32_t {
	kEsSuccess = 0, //!< `ESUCCESS`.
	kEsBadf = 8, //!< `EBADF`.
	kEsInval = 28, //!< `EINVAL`.
	kEsNosys = 52 //!< `ENOSYS`: recognised and deliberately not implemented.
};

/**
 * \brief Returns the byte `random_get` writes at `index`.
 * \param index The offset within the guest buffer.
 * \return The deterministic byte value.
 */
inline std::uint8_t randomByte(std::uint32_t index)
{
	return static_cast<std::uint8_t>(0xA5u ^ (index * 37u));
}

/**
 * \brief Returns the linear memory an import is called from.
 * \param vm The caller's context, or null.
 * \param index Which memory to look at.
 * \return The memory base, or null when there is none.
 */
inline std::uint8_t* linearMemory(WASM::VMContext* vm, std::uint32_t index = 0)
{
	if (vm == nullptr || index >= vm->memoryCount || vm->memories[index] == nullptr)
		return nullptr;
	return vm->memories[index]->memoryBase;
}

/**
 * \brief `fd_write`: appends the iovec bytes to the fixture sink.
 * \param vm The caller's context.
 * \param fd The descriptor the guest wrote to.
 * \param iovs Address of the iovec array in guest memory.
 * \param iovsLen Number of iovecs.
 * \param nwritten Address receiving the byte count.
 * \return `ESUCCESS`, or `EBADF` when the guest has no memory.
 */
inline std::int32_t fdWrite(WASM::VMContext* vm, std::int32_t fd,
		std::int32_t iovs, std::int32_t iovsLen, std::int32_t nwritten)
{
	std::uint8_t* mem = linearMemory(vm);
	if (mem == nullptr)
		return kEsBadf;
	++state().fdWriteCalls;
	std::uint32_t total = 0;
	for (std::int32_t i = 0; i < iovsLen; ++i) {
		std::uint32_t ptr = 0;
		std::uint32_t len = 0;
		std::memcpy(&ptr, mem + iovs + i * 8, 4);
		std::memcpy(&len, mem + iovs + i * 8 + 4, 4);
		if (fd == 1)
			state().stdoutBytes.append(
					reinterpret_cast<const char*>(mem) + ptr, len);
		else if (fd == 2)
			state().stderrBytes.append(
					reinterpret_cast<const char*>(mem) + ptr, len);
		total += len;
	}
	std::memcpy(mem + nwritten, &total, 4);
	return kEsSuccess;
}

/**
 * \brief `fd_read`: reports end of file.
 * \param vm The caller's context.
 * \param fd The descriptor (ignored).
 * \param iovs The iovec array (ignored).
 * \param iovsLen The iovec count (ignored).
 * \param nread Address receiving the byte count.
 * \return `ESUCCESS`, or `EBADF` when the guest has no memory.
 */
inline std::int32_t fdRead(WASM::VMContext* vm, std::int32_t fd,
		std::int32_t iovs, std::int32_t iovsLen, std::int32_t nread)
{
	(void)fd;
	(void)iovs;
	(void)iovsLen;
	std::uint8_t* mem = linearMemory(vm);
	if (mem == nullptr)
		return kEsBadf;
	const std::uint32_t zero = 0;
	std::memcpy(mem + nread, &zero, 4);
	return kEsSuccess;
}

/**
 * \brief `fd_close`: accepted and forgotten.
 * \param vm The caller's context (unused).
 * \param fd The descriptor (unused).
 * \return `ESUCCESS`.
 */
inline std::int32_t fdClose(WASM::VMContext* vm, std::int32_t fd)
{
	(void)vm;
	(void)fd;
	return kEsSuccess;
}

/**
 * \brief `fd_seek`: reports offset zero.
 * \param vm The caller's context.
 * \param fd The descriptor (unused).
 * \param offset The requested offset (unused).
 * \param whence The requested origin (unused).
 * \param newoffset Address receiving the new offset.
 * \return `ESUCCESS`, or `EBADF` when the guest has no memory.
 */
inline std::int32_t fdSeek(WASM::VMContext* vm, std::int32_t fd,
		std::int64_t offset, std::int32_t whence, std::int32_t newoffset)
{
	(void)fd;
	(void)offset;
	(void)whence;
	std::uint8_t* mem = linearMemory(vm);
	if (mem == nullptr)
		return kEsBadf;
	const std::uint64_t zero = 0;
	std::memcpy(mem + newoffset, &zero, 8);
	return kEsSuccess;
}

/**
 * \brief `fd_fdstat_get`: reports a character device, never a real file.
 * \param vm The caller's context.
 * \param fd The descriptor (unused).
 * \param out Address of the 24-byte fdstat record.
 * \return `ESUCCESS`, or `EBADF` when the guest has no memory.
 */
inline std::int32_t fdFdstatGet(WASM::VMContext* vm, std::int32_t fd,
		std::int32_t out)
{
	(void)fd;
	std::uint8_t* mem = linearMemory(vm);
	if (mem == nullptr)
		return kEsBadf;
	std::memset(mem + out, 0, 24);
	mem[out] = 2; //!< filetype: character_device
	return kEsSuccess;
}

/**
 * \brief `fd_prestat_get`: the stub preopens nothing.
 * \param vm The caller's context (unused).
 * \param fd The descriptor (unused).
 * \return `EBADF`.
 */
inline std::int32_t fdPrestatGet(WASM::VMContext* vm, std::int32_t fd)
{
	(void)vm;
	(void)fd;
	return kEsBadf;
}

/**
 * \brief `fd_prestat_dir_name`: the stub preopens nothing.
 * \param vm The caller's context (unused).
 * \param fd The descriptor (unused).
 * \param path The path buffer (unused).
 * \param pathLen The path buffer length (unused).
 * \return `EBADF`.
 */
inline std::int32_t fdPrestatDirName(WASM::VMContext* vm, std::int32_t fd,
		std::int32_t path, std::int32_t pathLen)
{
	(void)vm;
	(void)fd;
	(void)path;
	(void)pathLen;
	return kEsBadf;
}

/**
 * \brief `args_sizes_get`: reports one argument, `test`.
 * \param vm The caller's context.
 * \param argcOut Address receiving the argument count.
 * \param bufSizeOut Address receiving the byte size of the argument bytes.
 * \return `ESUCCESS`, or `EBADF` when the guest has no memory.
 */
inline std::int32_t argsSizesGet(WASM::VMContext* vm, std::int32_t argcOut,
		std::int32_t bufSizeOut)
{
	std::uint8_t* mem = linearMemory(vm);
	if (mem == nullptr)
		return kEsBadf;
	const std::uint32_t argc = 1;
	const std::uint32_t size = 5; //!< `"test\0"`.
	std::memcpy(mem + argcOut, &argc, 4);
	std::memcpy(mem + bufSizeOut, &size, 4);
	return kEsSuccess;
}

/**
 * \brief `args_get`: writes the single `test` argument.
 * \param vm The caller's context.
 * \param argv Address receiving the argument pointer array.
 * \param buf Address receiving the argument bytes.
 * \return `ESUCCESS`, or `EBADF` when the guest has no memory.
 */
inline std::int32_t argsGet(WASM::VMContext* vm, std::int32_t argv,
		std::int32_t buf)
{
	std::uint8_t* mem = linearMemory(vm);
	if (mem == nullptr)
		return kEsBadf;
	const std::uint32_t ptr = static_cast<std::uint32_t>(buf);
	std::memcpy(mem + argv, &ptr, 4);
	std::memcpy(mem + buf, "test", 5);
	return kEsSuccess;
}

/**
 * \brief `environ_sizes_get`: the environment is empty.
 * \param vm The caller's context.
 * \param countOut Address receiving the variable count.
 * \param bufSizeOut Address receiving the byte size of the environment.
 * \return `ESUCCESS`, or `EBADF` when the guest has no memory.
 */
inline std::int32_t environSizesGet(WASM::VMContext* vm, std::int32_t countOut,
		std::int32_t bufSizeOut)
{
	std::uint8_t* mem = linearMemory(vm);
	if (mem == nullptr)
		return kEsBadf;
	const std::uint32_t zero = 0;
	std::memcpy(mem + countOut, &zero, 4);
	std::memcpy(mem + bufSizeOut, &zero, 4);
	return kEsSuccess;
}

/**
 * \brief `environ_get`: the environment is empty, so nothing is written.
 * \param vm The caller's context (unused).
 * \param environ The pointer array (unused).
 * \param buf The byte buffer (unused).
 * \return `ESUCCESS`.
 */
inline std::int32_t environGet(WASM::VMContext* vm, std::int32_t environ,
		std::int32_t buf)
{
	(void)vm;
	(void)environ;
	(void)buf;
	return kEsSuccess;
}

/**
 * \brief `clock_time_get`: reports the fixed fixture tick count.
 * \param vm The caller's context.
 * \param clockId The clock the guest asked for (ignored).
 * \param precision The requested precision (ignored).
 * \param out Address receiving the 64-bit tick count.
 * \return `ESUCCESS`, or `EBADF` when the guest has no memory.
 */
inline std::int32_t clockTimeGet(WASM::VMContext* vm, std::int32_t clockId,
		std::int64_t precision, std::int32_t out)
{
	(void)clockId;
	(void)precision;
	std::uint8_t* mem = linearMemory(vm);
	if (mem == nullptr)
		return kEsBadf;
	++state().clockCalls;
	const std::uint64_t ticks = state().clockTicks;
	std::memcpy(mem + out, &ticks, 8);
	return kEsSuccess;
}

/**
 * \brief `random_get`: fills a fixed pattern, never host entropy.
 * \param vm The caller's context.
 * \param buf Address receiving the bytes.
 * \param len Number of bytes.
 * \return `ESUCCESS`, or `EBADF` when the guest has no memory.
 */
inline std::int32_t randomGet(WASM::VMContext* vm, std::int32_t buf,
		std::int32_t len)
{
	std::uint8_t* mem = linearMemory(vm);
	if (mem == nullptr)
		return kEsBadf;
	++state().randomCalls;
	for (std::int32_t i = 0; i < len; ++i)
		mem[buf + i] = randomByte(static_cast<std::uint32_t>(i));
	return kEsSuccess;
}

/**
 * \brief `sched_yield`: accepted and ignored.
 * \param vm The caller's context (unused).
 * \return `ESUCCESS`.
 */
inline std::int32_t schedYield(WASM::VMContext* vm)
{
	(void)vm;
	return kEsSuccess;
}

/**
 * \brief `proc_exit`: records the code and raises an internal trap.
 * \param vm The caller's context (unused).
 * \param code The guest's exit code.
 * \return Never returns normally; the trap unwinds the call.
 */
inline std::int32_t procExit(WASM::VMContext* vm, std::int32_t code)
{
	(void)vm;
	state().exitCalled = true;
	state().exitCode = code;
	WASM::trap(WASM::TrapKind::Internal, "wasi stub proc_exit");
	return 0; //!< unreachable; the wrapper reports the trap
}

/**
 * \brief `poll_oneoff`: reports no events, never waits.
 * \param vm The caller's context.
 * \param in The subscription array (ignored).
 * \param out The event array (ignored).
 * \param count The subscription count (ignored).
 * \param nevents Address receiving the event count.
 * \return `ESUCCESS`, or `EBADF` when the guest has no memory.
 */
inline std::int32_t pollOneoff(WASM::VMContext* vm, std::int32_t in,
		std::int32_t out, std::int32_t count, std::int32_t nevents)
{
	(void)in;
	(void)out;
	(void)count;
	std::uint8_t* mem = linearMemory(vm);
	if (mem == nullptr)
		return kEsBadf;
	const std::uint32_t zero = 0;
	std::memcpy(mem + nevents, &zero, 4);
	return kEsSuccess;
}

/**
 * \brief Returns `ESUCCESS` for a two-argument import.
 * \param vm The caller's context (unused).
 * \param a First argument (unused).
 * \param b Second argument (unused).
 * \return `ESUCCESS`.
 */
inline std::int32_t ok2(WASM::VMContext* vm, std::int32_t a, std::int32_t b)
{
	(void)vm;
	(void)a;
	(void)b;
	return kEsSuccess;
}

/**
 * \brief Returns `ENOSYS` for a three-argument import, counting the refusal.
 * \param vm The caller's context (unused).
 * \param a First argument (unused).
 * \param b Second argument (unused).
 * \param c Third argument (unused).
 * \return `ENOSYS`.
 */
inline std::int32_t unsupported3(WASM::VMContext* vm, std::int32_t a,
		std::int32_t b, std::int32_t c)
{
	(void)vm;
	(void)a;
	(void)b;
	(void)c;
	++state().refusedCalls;
	return kEsNosys;
}

/**
 * \brief Returns `ENOSYS` for a five-argument import, counting the refusal.
 * \param vm The caller's context (unused).
 * \param a First argument (unused).
 * \param b Second argument (unused).
 * \param c Third argument (unused).
 * \param d Fourth argument (unused).
 * \param e Fifth argument (unused).
 * \return `ENOSYS`.
 */
inline std::int32_t unsupported5(WASM::VMContext* vm, std::int32_t a,
		std::int32_t b, std::int32_t c, std::int32_t d, std::int32_t e)
{
	(void)vm;
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	++state().refusedCalls;
	return kEsNosys;
}

/**
 * \brief Returns `ENOSYS` for a nine-argument import, counting the refusal.
 * \param vm The caller's context (unused).
 * \param a First argument (unused).
 * \param b Second argument (unused).
 * \param c Third argument (unused).
 * \param d Fourth argument (unused).
 * \param e Fifth argument (unused).
 * \param f Sixth argument (unused).
 * \param g Seventh argument (unused).
 * \param h Eighth argument (unused).
 * \param i Ninth argument (unused).
 * \return `ENOSYS`.
 */
inline std::int32_t unsupported9(WASM::VMContext* vm, std::int32_t a,
		std::int32_t b, std::int32_t c, std::int32_t d, std::int32_t e,
		std::int64_t f, std::int64_t g, std::int32_t h, std::int32_t i)
{
	(void)vm;
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	(void)f;
	(void)g;
	(void)h;
	(void)i;
	++state().refusedCalls;
	return kEsNosys;
}

/**
 * \brief Registers the fake `wasi_snapshot_preview1` module.
 * \param imports The resolver the instance will use.
 */
inline void registerPreview1(WASM::RegistryImportResolver& imports)
{
	const char* module = "wasi_snapshot_preview1";
	imports.registerFunction(module, "fd_write", WASM::nativeImport<&fdWrite>());
	imports.registerFunction(module, "fd_read", WASM::nativeImport<&fdRead>());
	imports.registerFunction(module, "fd_close", WASM::nativeImport<&fdClose>());
	imports.registerFunction(module, "fd_seek", WASM::nativeImport<&fdSeek>());
	imports.registerFunction(module, "fd_fdstat_get",
			WASM::nativeImport<&fdFdstatGet>());
	imports.registerFunction(module, "fd_prestat_get",
			WASM::nativeImport<&fdPrestatGet>());
	imports.registerFunction(module, "fd_prestat_dir_name",
			WASM::nativeImport<&fdPrestatDirName>());
	imports.registerFunction(module, "args_sizes_get",
			WASM::nativeImport<&argsSizesGet>());
	imports.registerFunction(module, "args_get", WASM::nativeImport<&argsGet>());
	imports.registerFunction(module, "environ_sizes_get",
			WASM::nativeImport<&environSizesGet>());
	imports.registerFunction(module, "environ_get",
			WASM::nativeImport<&environGet>());
	imports.registerFunction(module, "clock_time_get",
			WASM::nativeImport<&clockTimeGet>());
	imports.registerFunction(module, "random_get",
			WASM::nativeImport<&randomGet>());
	imports.registerFunction(module, "sched_yield",
			WASM::nativeImport<&schedYield>());
	imports.registerFunction(module, "proc_exit",
			WASM::nativeImport<&procExit>());
	imports.registerFunction(module, "poll_oneoff",
			WASM::nativeImport<&pollOneoff>());
	imports.registerFunction(module, "fd_filestat_get",
			WASM::nativeImport<&ok2>());
	imports.registerFunction(module, "fd_fdstat_set_flags",
			WASM::nativeImport<&ok2>());
	imports.registerFunction(module, "fd_sync", WASM::nativeImport<&ok2>());
	imports.registerFunction(module, "fd_renumber", WASM::nativeImport<&ok2>());
	//! Nothing below is faked with a real object: a fixture that tries to
	//! open, rename or unlink a path gets `ENOSYS`, and the refusal is counted.
	imports.registerFunction(module, "path_open",
			WASM::nativeImport<&unsupported9>());
	imports.registerFunction(module, "path_remove_directory",
			WASM::nativeImport<&unsupported3>());
	imports.registerFunction(module, "path_unlink_file",
			WASM::nativeImport<&unsupported3>());
	imports.registerFunction(module, "path_rename",
			WASM::nativeImport<&unsupported5>());
}

/**
 * \brief Builds a host function with the declared flat signature.
 * \param params The flattened parameter types the import declares.
 * \param results The flattened result types the import declares.
 * \return A `ComponentFunction` that satisfies the signature check and writes
 * nothing; callers replace `callValues` when the stub must be called.
 */
inline WASM::ComponentFunction zeroFunction(
		std::vector<WASM::AbiCoreType> params,
		std::vector<WASM::AbiCoreType> results)
{
	WASM::ComponentFunction function;
	function.params = std::move(params);
	function.results = std::move(results);
	function.callValues = [](std::span<const WASM::ComponentValue>,
			std::span<WASM::ComponentValue>) {};
	return function;
}

} // namespace WasiStub

#endif // WASISTUB_HPP
