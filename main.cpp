#include <iostream>
#include <binaryen-c.h>
#include <jit/jit.h>
#include <map>

using namespace std;

typedef std::map<BinaryenType, jit_type_t> BinaryenToJitMap;

void RegisterBinaryenTypesToLibjit(BinaryenToJitMap& typemap)
{
	// a
	typemap.insert_or_assign(BinaryenTypeNone(),jit_type_void);
	typemap.insert_or_assign(BinaryenTypeInt32(),jit_type_int);
	typemap.insert_or_assign(BinaryenTypeInt64(),jit_type_long);
	typemap.insert_or_assign(BinaryenTypeFloat32(),jit_type_float32);
	typemap.insert_or_assign(BinaryenTypeFloat64(),jit_type_float64);
	/// TODO!
	typemap.insert_or_assign(BinaryenTypeVec128(),jit_type_void);

	typemap.insert_or_assign(BinaryenTypeFuncref(),jit_type_void_ptr);
	typemap.insert_or_assign(BinaryenTypeExternref(),jit_type_void_ptr);
	typemap.insert_or_assign(BinaryenTypeAnyref(),jit_type_void_ptr);
	typemap.insert_or_assign(BinaryenTypeEqref(),jit_type_void_ptr);
	typemap.insert_or_assign(BinaryenTypeI31ref(),jit_type_void_ptr);
	typemap.insert_or_assign(BinaryenTypeStructref(),jit_type_void_ptr);
	typemap.insert_or_assign(BinaryenTypeArrayref(),jit_type_void_ptr);
	typemap.insert_or_assign(BinaryenTypeStringref(),jit_type_void_ptr);
	typemap.insert_or_assign(BinaryenTypeNullref(),jit_type_void_ptr);
	typemap.insert_or_assign(BinaryenTypeNullExternref(),jit_type_void_ptr);
	typemap.insert_or_assign(BinaryenTypeNullFuncref(),jit_type_void_ptr);
	typemap.insert_or_assign(BinaryenTypeUnreachable(),jit_type_void);
}

int main()
{
	BinaryenModuleRef module = BinaryenModuleCreate();

	// Create a function type for  i32 (i32, i32)
	BinaryenType ii[2] = {BinaryenTypeInt32(), BinaryenTypeInt32()};
	BinaryenType params = BinaryenTypeCreate(ii, 2);
	BinaryenType results = BinaryenTypeInt32();

	// Get the 0 and 1 arguments, and add them
	BinaryenExpressionRef x = BinaryenLocalGet(module, 0, BinaryenTypeInt32()),
		y = BinaryenLocalGet(module, 1, BinaryenTypeInt32());
	BinaryenExpressionRef add = BinaryenBinary(module, BinaryenAddInt32(), x, y);

	// Create the add function
	// Note: no additional local variables
	// Note: no basic blocks here, we are an AST. The function body is just an
	// expression node.
	BinaryenFunctionRef adder =
		BinaryenAddFunction(module, "adder", params, results, NULL, 0, add);

	// Print it out
	BinaryenModulePrint(module);

	// Clean up the module, which owns all the objects we created above
	BinaryenModuleDispose(module);

	return 0;
}
