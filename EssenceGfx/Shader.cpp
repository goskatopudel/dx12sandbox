#include <D3Dcompiler.h>
#include "Shader.h"
#include "Array.h"
#include "Freelist.h"
#include "Hashmap.h"
#include "Files.h"
#include "Debug.h"
#include "Commands.h"

namespace Essence {

struct shader_macro_t {
	const char*	name;
	const char* define;
};

struct ShaderDefines {
	ShaderDefines() {}
	ShaderDefines(std::initializer_list<shader_macro_t> macros) {
		Array<shader_macro_t> list(GetThreadScratchAllocator());
		Reserve(list, macros.size());
		for (auto item : macros) {
			PushBack(list, item);
		}
		quicksort(list.DataPtr, 0, Size(list), [](shader_macro_t a, shader_macro_t b) {
			return strcmp(a.name, b.name) < 0;
		});
		
	}
};

const char *GetProfileStr(ShaderProfileEnum profile) {
	switch (profile) {
	case VS_5_0:
		return "vs_5_0";
	case VS_5_1:
		return "vs_5_1";
	case PS_5_0:
		return "ps_5_0";
	case PS_5_1:
		return "ps_5_1";
	case CS_5_0:
		return "cs_5_0";
	case CS_5_1:
		return "cs_5_1";
	}
	return "";
}

struct shader_key_t {
	ResourceNameId		file;
	TextId				function;
	ShaderProfileEnum	profile;
};

struct shader_record_t {
	shader_key_t		key;
	shader_metadata_t	metadata;
};

void Compile(shader_handle handle, shader_key_t const& desc);

Hashmap<shader_key_t, shader_handle>		ShadersIndex;
Freelist<shader_record_t, shader_handle>	ShadersTable;
Array<shader_bytecode_t>					ShadersFastData;
RWLock										ReadWriteLock;

IAllocator*									BytecodeAllocator = GetMallocAllocator();

shader_handle		Create(shader_key_t const& key) {
	auto handle = Create(ShadersTable);
	Set(ShadersIndex, key, handle);

	ShadersTable[handle] = {};
	ShadersTable[handle].key = key;

	if (Size(ShadersFastData) < handle.GetIndex() + 1) {
		Resize(ShadersFastData, handle.GetIndex() + 1);
	}
	ShadersFastData[handle.GetIndex()] = {};

	return handle;
}

shader_handle		GetShader(ResourceNameId file, TextId function, ShaderProfileEnum profile) {
	shader_key_t key;
	key.file = file;
	key.function = function;
	key.profile = profile;

	ReadWriteLock.LockShared();

	auto handlePtr = Get(ShadersIndex, key);
	if (handlePtr) {
		auto handle = *handlePtr;
		ReadWriteLock.UnlockShared();
		return handle;
	}
	ReadWriteLock.UnlockShared();
	ReadWriteLock.LockExclusive();

	auto handle = Create(key);

	Compile(handle, key);

	ReadWriteLock.UnlockExclusive();

	return handle;
}

shader_bytecode_t	GetShaderBytecode(shader_handle shader) {
	Check(Contains(ShadersTable, shader));
	return ShadersFastData[shader.GetIndex()];
}

AString	GetShaderDisplayString(shader_handle shader) {
	return Format("%s:%s()", (cstr)GetString(ShadersTable[shader].key.file), (cstr)GetString(ShadersTable[shader].key.function));
}

void*	Copy(IAllocator* allocator, const void* src, u64 bytesize) {
	auto dst = allocator->Allocate(bytesize, 1);
	memcpy(dst, src, bytesize);
	return dst;
}

void Compile(shader_handle handle, shader_key_t const& desc) {
	ID3DBlob *codeBlob = nullptr;
	ID3DBlob *errBlob = nullptr;

	auto shaderCode = ReadEntireFile(GetString(desc.file));

	auto compileHresult = D3DCompile2(shaderCode.data_ptr, shaderCode.bytesize, GetString(desc.file), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, GetString(desc.function), GetProfileStr(desc.profile),
		0, 0, 0, nullptr, 0, &codeBlob, &errBlob);

	Check(Contains(ShadersTable, handle));

	auto& record = ShadersTable[handle];
	auto& fast = ShadersFastData[handle.GetIndex()];

	if (codeBlob != nullptr) {
		if (fast.bytecode) {
			BytecodeAllocator->Free(fast.bytecode);
			fast = {};

			record.metadata.recompiled = 1;
		}

		fast.bytecode = Copy(BytecodeAllocator, codeBlob->GetBufferPointer(), codeBlob->GetBufferSize());
		fast.bytesize = codeBlob->GetBufferSize();
		fast.bytecode_hash = Hash::MurmurHash2_64(fast.bytecode, (i32)codeBlob->GetBufferSize(), 0);


		ID3D12ShaderReflection* pReflection;
		VerifyHr(D3DReflect(fast.bytecode, fast.bytesize, IID_PPV_ARGS(&pReflection)));

		D3D12_SHADER_DESC shaderDesc;
		VerifyHr(pReflection->GetDesc(&shaderDesc));

		ComRelease(pReflection);
	}

	if (errBlob != nullptr) {
		auto shaderString = Format("%s(%s)", (const char*)GetString(desc.file), (const char*)GetString(desc.function));

		if (codeBlob != nullptr) {
			debugf(Format("%s compilation warnings!\n%s", (const char*)shaderString, (char*)errBlob->GetBufferPointer()));
		}
		else {
			debugf(Format("%s compilation failed!\n%s", (const char*)shaderString, (char*)errBlob->GetBufferPointer()));
		}
	}

	FreeMemory(shaderCode);

	ComRelease(codeBlob);
	ComRelease(errBlob);
}

shader_metadata_t	GetShaderMetadata(shader_handle shader) {
	ReaderScope readScope(&ReadWriteLock);
	return ShadersTable[shader].metadata;
}

void ReloadShaders() {
	ReadWriteLock.LockExclusive();

	for (auto kv : ShadersIndex) {
		Compile(kv.value, kv.key);
	}

	ReadWriteLock.UnlockExclusive();

	FlushShaderChanges();

	ReadWriteLock.LockExclusive();

	for (auto kv : ShadersIndex) {
		ShadersTable[kv.value].metadata.recompiled = 0;
	}

	ReadWriteLock.UnlockExclusive();
}

void FreeShadersMemory() {
	for (auto &f : ShadersFastData) {
		BytecodeAllocator->Free(f.bytecode);
	}

	FreeMemory(ShadersIndex);
	FreeMemory(ShadersTable);
	FreeMemory(ShadersFastData);
}

}