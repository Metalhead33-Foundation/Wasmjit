#include <iostream>
#include "Io/ElvDataStream.hpp"
#include "Io/EuphFile.hpp"
#include "Base/WasmModule.hpp"

using namespace std;
typedef Euph::Io::File RegularFile;
typedef Elv::Io::DataStream<Elv::Util::Endian::Little> WasmStream;
#define _WASMPATH "/home/legacy/helloworld3.wasm"
#define _WASMPATH "/tmp/app.wasm"
static const char* WASMPATH = _WASMPATH;

int main()
{
	RegularFile f(WASMPATH, Elv::Io::Mode::READ);
	std::cout << WASMPATH << "\nSize: " << f.size() << std::endl;

	WASM::Module mod;
	mod.fromFile(f);
	for(const auto& it : mod.getSections())
	{
		std::cout << "Section type: " << static_cast<int>(it.type) << " (" << WASM::getSectionTypeName(it.type) << ")\n"
				  << "Section offset: " << it.offset << "\nSection size: " << it.size << "\n===\n";
	}
	std::cout << std::endl;

	return 0;
}
