// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "ShaderParser.h"
#include "core/ConfigParser.h"
#include "core/Log.h"

#include <string_view>

using namespace Graphics::ShaderParser;

Config::Parser::Result ParserContext::operator()(Parser *parser, const Token &tok)
{
	if (m_shaderData->name.empty() && !tok.isKeyword("Shader")) {
		Log::Warning("{} Expected Shader \"<name>\" directive as first statement in file.",
			parser->MakeLineInfo());
		m_shaderData->name = "<unknown>";
	}

	if (tok.isKeyword("Shader")) {
		Token name;

		if (!parser->Acquire(Token::Identifier, &name))
			return Result::DidNotMatch;

		m_shaderData->name = name.contents();
		return Result::Parsed;
	}

	if (tok.isKeyword("Texture"))
		return parseTexture(parser);

	if (tok.isKeyword("Buffer"))
		return parseBuffer(parser);

	if (tok.isKeyword("PushConstant"))
		return parsePushConstant(parser);

	if (tok.isKeyword("Vertex")) {
		Token path;
		if (!parser->Acquire(Token::String, &path))
			return Result::DidNotMatch;

		m_shaderData->vertexPath = path.contents();
		return Result::Parsed;
	}

	if (tok.isKeyword("Fragment")) {
		Token path;
		if (!parser->Acquire(Token::String, &path))
			return Result::DidNotMatch;

		m_shaderData->fragmentPath = path.contents();
		return Result::Parsed;
	}

	// tessellation and compute stages (OpenGL 4)
	if (tok.isKeyword("TessControl") || tok.isKeyword("TessEval") || tok.isKeyword("Compute")) {
		Token path;
		if (!parser->Acquire(Token::String, &path))
			return Result::DidNotMatch;

		std::string &stage = tok.isKeyword("TessControl") ? m_shaderData->tessControlPath :
			(tok.isKeyword("TessEval") ? m_shaderData->tessEvalPath : m_shaderData->computePath);
		stage = path.contents();
		return Result::Parsed;
	}

	if (tok.isKeyword("Image"))
		return parseImage(parser);

	// If we get an identifier we don't know what to do with, complain about it and skip the line.
	Log::Warning("{} Expected one of 'Shader', 'Texture', 'Image', 'Buffer', 'PushConstant', 'Fragment', 'Vertex', 'TessControl', 'TessEval', 'Compute'; got '{}'\n",
		parser->MakeLineInfo(), tok.range);
	return Result::DidNotMatch;
}

Config::Parser::Result ParserContext::parseTexture(Parser *parser)
{
	Token type, bindName;
	if (!parser->Acquire(Token::Identifier, &type) || !parser->Acquire(Token::Identifier, &bindName))
		return Result::ParseFailure;

	TextureInfo texInfo {};

	if (type.isKeyword("sampler2D")) {
		texInfo.type = Graphics::TextureType::TEXTURE_2D;
	} else if (type.isKeyword("sampler2DArray")) {
		texInfo.type = Graphics::TextureType::TEXTURE_2D_ARRAY;
	} else if (type.isKeyword("samplerCube")) {
		texInfo.type = Graphics::TextureType::TEXTURE_CUBE_MAP;
	} else {
		Log::Warning("{} Expected one of 'sampler2D, sampler2DArray, samplerCube', got '{}'\n",
			parser->MakeLineInfo(), type.range);
		return Result::ParseFailure;
	}

	texInfo.bindName = bindName.toString();
	texInfo.binding = UINT32_MAX;

	while (parser->IsLineRemaining()) {

		Token attrib;
		if (!parser->Acquire(Token::Identifier, &attrib))
			return Result::ParseFailure;

		if (attrib.isKeyword("name")) {
			Token name;
			if (!parser->Symbol('=') || !parser->Acquire(Token::Identifier, &name))
				return Result::ParseFailure;

			texInfo.name = name.toString();
		} else if (attrib.isKeyword("binding")) {
			Token id;
			if (!parser->Symbol('=') || !parser->Acquire(Token::Number, &id))
				return Result::ParseFailure;

			texInfo.binding = id.value;
			m_nextTextureBinding = texInfo.binding + 1;
		} else {
			Log::Warning("{} Unexpected attribute {} in texture binding.",
				parser->MakeLineInfo(), attrib.range);
			return Result::ParseFailure;
		}

	}

	// No explicit texture binding index, use the auto-generated binding index instead.
	if (texInfo.binding == UINT32_MAX) {
		texInfo.binding = m_nextTextureBinding++;
	}

	m_shaderData->textureBindings.emplace_back(std::move(texInfo));
	return Result::Parsed;

}

// Image image2D <bindName> [name=<name>] [binding=<n>] format=<rgba8|rgba16f|rgba32f|r32f> [access=<read|write|readwrite>]
// An image a shader reads or writes texel by texel, as compute does. It takes a texture unit's binding like a texture,
// and the material's SetTexture sets it.
Config::Parser::Result ParserContext::parseImage(Parser *parser)
{
	Token type, bindName;
	if (!parser->Acquire(Token::Identifier, &type) || !parser->Acquire(Token::Identifier, &bindName))
		return Result::ParseFailure;

	TextureInfo texInfo {};
	texInfo.image = true;
	if (type.isKeyword("image2D")) {
		texInfo.type = Graphics::TextureType::TEXTURE_2D;
	} else if (type.isKeyword("image2DArray")) {
		texInfo.type = Graphics::TextureType::TEXTURE_2D_ARRAY;
	} else {
		Log::Warning("{} Expected one of 'image2D, image2DArray', got '{}'\n", parser->MakeLineInfo(), type.range);
		return Result::ParseFailure;
	}

	texInfo.bindName = bindName.toString();
	texInfo.binding = UINT32_MAX;

	while (parser->IsLineRemaining()) {
		Token attrib, value;
		if (!parser->Acquire(Token::Identifier, &attrib) || !parser->Symbol('='))
			return Result::ParseFailure;

		if (attrib.isKeyword("binding")) {
			if (!parser->Acquire(Token::Number, &value))
				return Result::ParseFailure;
			texInfo.binding = value.value;
			m_nextTextureBinding = texInfo.binding + 1;
			continue;
		}

		if (!parser->Acquire(Token::Identifier, &value))
			return Result::ParseFailure;

		if (attrib.isKeyword("name")) {
			texInfo.name = value.toString();
		} else if (attrib.isKeyword("format")) {
			if (value.isKeyword("rgba8"))
				texInfo.imageFormat = Graphics::TEXTURE_RGBA_8888;
			else if (value.isKeyword("rgba16f"))
				texInfo.imageFormat = Graphics::TEXTURE_RGBA16F;
			else if (value.isKeyword("rgba32f"))
				texInfo.imageFormat = Graphics::TEXTURE_RGBA32F;
			else if (value.isKeyword("r32f"))
				texInfo.imageFormat = Graphics::TEXTURE_R32F;
			else {
				Log::Warning("{} Expected one of 'rgba8, rgba16f, rgba32f, r32f', got '{}'\n", parser->MakeLineInfo(), value.range);
				return Result::ParseFailure;
			}
		} else if (attrib.isKeyword("access")) {
			if (value.isKeyword("read"))
				texInfo.imageAccess = IMAGE_READ;
			else if (value.isKeyword("write"))
				texInfo.imageAccess = IMAGE_WRITE;
			else if (value.isKeyword("readwrite"))
				texInfo.imageAccess = IMAGE_READ_WRITE;
			else {
				Log::Warning("{} Expected one of 'read, write, readwrite', got '{}'\n", parser->MakeLineInfo(), value.range);
				return Result::ParseFailure;
			}
		} else {
			Log::Warning("{} Unexpected attribute {} in image binding.", parser->MakeLineInfo(), attrib.range);
			return Result::ParseFailure;
		}
	}

	if (texInfo.imageFormat == Graphics::TEXTURE_NONE) {
		Log::Warning("{} An image needs a format.", parser->MakeLineInfo());
		return Result::ParseFailure;
	}

	if (texInfo.binding == UINT32_MAX)
		texInfo.binding = m_nextTextureBinding++;

	m_shaderData->textureBindings.emplace_back(std::move(texInfo));
	return Result::Parsed;
}

Config::Parser::Result ParserContext::parseBuffer(Parser *parser)
{
	Token name;
	if (!parser->Acquire(Token::Identifier, &name))
		return Result::ParseFailure;

	BufferInfo bufInfo{};
	bufInfo.name = name.toString();

	if (parser->IsLineRemaining()) {
		Token binding;
		if (!parser->Keyword("binding") || !parser->Symbol('=') || !parser->Acquire(Token::Number, &binding))
			return Result::ParseFailure;

		bufInfo.binding = binding.value;
		m_nextBufferBinding = bufInfo.binding + 1;
	} else {
		bufInfo.binding = m_nextBufferBinding++;
	}

	m_shaderData->bufferBindings.emplace_back(std::move(bufInfo));
	return Result::Parsed;
}

Config::Parser::Result ParserContext::parsePushConstant(Parser *parser)
{
	Token type;
	if (!parser->Acquire(Token::Identifier, &type))
		return Result::ParseFailure;

	PushConstantInfo constInfo{};
	if (type.isKeyword("int")) {
		constInfo.type = Graphics::ConstantDataFormat::DATA_FORMAT_INT;
	} else if (type.isKeyword("float")) {
		constInfo.type = Graphics::ConstantDataFormat::DATA_FORMAT_FLOAT;
	} else if (type.isKeyword("vec3")) {
		constInfo.type = Graphics::ConstantDataFormat::DATA_FORMAT_FLOAT3;
	} else if (type.isKeyword("vec4")) {
		constInfo.type = Graphics::ConstantDataFormat::DATA_FORMAT_FLOAT4;
	} else if (type.isKeyword("mat3")) {
		constInfo.type = Graphics::ConstantDataFormat::DATA_FORMAT_MAT3;
	} else if (type.isKeyword("mat4")) {
		constInfo.type = Graphics::ConstantDataFormat::DATA_FORMAT_MAT4;
	} else {
		Log::Warning("{} Expected one of 'int', 'float', 'vec3', 'vec4', 'mat3', 'mat4'; got '{}'\n",
			parser->MakeLineInfo(), type.range);
		return Result::ParseFailure;
	}

	Token name;
	if (!parser->Acquire(Token::Identifier, &name))
		return Result::ParseFailure;

	constInfo.name = name.toString();

	if (parser->IsLineRemaining()) {
		Token binding;
		if (!parser->Keyword("binding") || !parser->Symbol('=') || !parser->Acquire(Token::Number, &binding))
			return Result::ParseFailure;

		constInfo.binding = binding.value;

		if (constInfo.binding < m_nextPushConstantBinding) {
			Log::Warning("{} PushConstant binding '{}' index {} is before next-free index {}. Assigning that index.\n",
				parser->MakeLineInfo(), constInfo.name, constInfo.binding, m_nextPushConstantBinding);
			constInfo.binding = m_nextPushConstantBinding;
		}

		for (const auto &info : m_shaderData->pushConstantBindings) {
			if (info.binding == constInfo.binding) {
				Log::Warning("{} PushConstant binding '{}' index {} is already taken by constant '{}'. Assigning index {}.\n",
					parser->MakeLineInfo(), constInfo.name, constInfo.binding, info.binding, m_nextPushConstantBinding);
				constInfo.binding = m_nextPushConstantBinding;
			}
		}

		m_nextPushConstantBinding = constInfo.binding + 1;
	} else {
		constInfo.binding = m_nextPushConstantBinding++;
	}

	m_shaderData->pushConstantBindings.emplace_back(std::move(constInfo));
	return Result::Parsed;
}
