#pragma once

#include "Essence.h"
#include "Freelist.h"
#include "Hash.h"

#pragma comment(lib,"d3dcompiler.lib")
#include <d3d12.h>

#define SHADER_(FILE, FUNC, PROFILE) Essence::GetShader(NAME_("shaders/" #FILE ".hlsl"), TEXT_(#FUNC), PROFILE)

namespace Essence {

typedef GenericHandle32<24, TYPE_ID(Shader)> shader_handle;

enum ShaderProfileEnum {
	VS_5_0,
	VS_5_1,
	PS_5_0,
	PS_5_1,
	CS_5_0,
	CS_5_1
};

struct shader_bytecode_t {
	u64		bytecode_hash;
	void*	bytecode;
	u64		bytesize;
};

shader_handle		GetShader(ResourceNameId file, TextId function, ShaderProfileEnum profile);
shader_bytecode_t	GetShaderBytecode(shader_handle shader);
AString	GetShaderDisplayString(shader_handle shader);

void ReloadShaders();
void FreeShadersMemory();

}