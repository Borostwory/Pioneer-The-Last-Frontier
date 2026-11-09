// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _GRAPHICS_UPLOADSTATS_H
#define _GRAPHICS_UPLOADSTATS_H

#include <cstddef>
#include <cstdint>

// What the renderer hands the graphics driver, for the frontier log (FrontierLog.cpp): how many calls, how many bytes
// and how much CPU time went into them, per frame. The time is what the calls take on the main thread, which is where
// the driver stalls show: a call that has to wait for the GPU, or for memory, takes long. Main thread only, as the GL
// calls are. Counts only while switched on (the log switches it on while it is written).
namespace Graphics {
	enum UploadKind {
		UPLOAD_BUFFER_NEW, // vertex and index buffers made, or their storage thrown away for new (orphaned)
		UPLOAD_BUFFER_MAP, // mapping a vertex or index buffer to write into (can wait for the GPU)
		UPLOAD_BUFFER_WRITE, // data into vertex and index buffers: unmapping (the copy) and BufferSubData
		UPLOAD_BUFFER_FREE, // vertex and index buffers deleted
		UPLOAD_UNIFORMS, // uniform buffers: the per draw data
		UPLOAD_TEXTURE_NEW, // textures made (their storage)
		UPLOAD_TEXTURE_WRITE, // data into textures (TexSubImage)
		UPLOAD_MIPMAPS, // mipmaps made from a texture after it changed
		UPLOAD_KINDS
	};

	struct UploadCount {
		uint32_t calls = 0;
		uint64_t bytes = 0;
		uint64_t nanos = 0;
	};

	// switched on: counts from now
	void SetUploadStatsEnabled(bool enabled);
	bool UploadStatsEnabled();
	// this frame's counts so far, into out[UPLOAD_KINDS], and starts again from 0
	void TakeUploadCounts(UploadCount *out);
	// short name of a kind, for the log
	const char *UploadKindName(UploadKind kind);
	// bytes of a texel, or of a 4x4 block for compressed formats; as an int so this header needs no Types.h
	size_t UploadTexelBytes(int textureFormat);

	// adds the time it lives to one call of a kind
	class UploadTimer {
	public:
		UploadTimer(UploadKind kind, size_t bytes);
		~UploadTimer();
		UploadTimer(const UploadTimer &) = delete;
		UploadTimer &operator=(const UploadTimer &) = delete;

	private:
		UploadKind m_kind;
		long long m_start; // 0: not counting
	};
} // namespace Graphics

#endif // _GRAPHICS_UPLOADSTATS_H
