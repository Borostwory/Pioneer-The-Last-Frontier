// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "UploadStats.h"

#include "graphics/Texture.h"

#include <algorithm>
#include <chrono>

namespace {
	bool s_enabled = false;
	Graphics::UploadCount s_counts[Graphics::UPLOAD_KINDS];

	long long NowNanos()
	{
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
} // namespace

namespace Graphics {
	void SetUploadStatsEnabled(const bool enabled)
	{
		s_enabled = enabled;
	}

	bool UploadStatsEnabled()
	{
		return s_enabled;
	}

	void TakeUploadCounts(UploadCount *out)
	{
		std::copy(std::begin(s_counts), std::end(s_counts), out);
		std::fill(std::begin(s_counts), std::end(s_counts), UploadCount());
	}

	const char *UploadKindName(const UploadKind kind)
	{
		switch (kind) {
		case UPLOAD_BUFFER_NEW: return "buffers made";
		case UPLOAD_BUFFER_MAP: return "buffers mapped";
		case UPLOAD_BUFFER_WRITE: return "buffer writes";
		case UPLOAD_BUFFER_FREE: return "buffers freed";
		case UPLOAD_UNIFORMS: return "uniforms";
		case UPLOAD_TEXTURE_NEW: return "textures made";
		case UPLOAD_TEXTURE_WRITE: return "texture writes";
		case UPLOAD_MIPMAPS: return "mipmaps";
		default: return "?";
		}
	}

	size_t UploadTexelBytes(const int textureFormat)
	{
		switch (TextureFormat(textureFormat)) {
		case TEXTURE_RGBA_8888: return 4;
		case TEXTURE_RGB_888: return 3;
		case TEXTURE_RG_88: return 2;
		case TEXTURE_R8: return 1;
		case TEXTURE_R32F: return 4;
		case TEXTURE_RG32F: return 8;
		case TEXTURE_RGBA16F: return 16; // uploaded as 32-bit floats
		case TEXTURE_RGBA32F: return 16;
		case TEXTURE_RGB10_A2: return 4;
		case TEXTURE_DXT1: return 8; // a block
		case TEXTURE_DXT5: return 16; // a block
		case TEXTURE_DEPTH: return 4;
		default: return 0;
		}
	}

	UploadTimer::UploadTimer(const UploadKind kind, const size_t bytes) :
		m_kind(kind),
		m_start(s_enabled ? NowNanos() : 0)
	{
		if (m_start) {
			s_counts[kind].calls++;
			s_counts[kind].bytes += bytes;
		}
	}

	UploadTimer::~UploadTimer()
	{
		if (m_start)
			s_counts[m_kind].nanos += uint64_t(std::max(NowNanos() - m_start, 0LL));
	}
} // namespace Graphics
