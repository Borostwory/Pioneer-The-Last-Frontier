// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _GEOPATCHCONTEXT_H
#define _GEOPATCHCONTEXT_H

#include <SDL_stdinc.h>

#include "Color.h"
#include "graphics/VertexBuffer.h"
#include "vector3.h"

#include <cstddef>
#include <deque>

// maximumpatch depth
#define GEOPATCH_MAX_DEPTH 15

// per-vertex surface data from the patch jobs: the colour, with the altitude
// above sea level in alpha, and the weights of the body's materials
// (TerrainMaterialTable, terrain-materials.glsl)
struct GeoVertexSurface {
	Color4ub col;
	Uint8 materials[8];
};

class GeoPatchContext : public RefCounted {
public:
	struct VBOVertex {
		vector3f pos;
		vector3f norm;
		Color4ub col;
		vector2f uv;
		Uint8 materials[8];
	};

	// position, normal, colour, uv, and the material weights as two normalised
	// UBYTE4 at locations 10 and 11 (a_materials0/1 in the terrain shaders)
	static Graphics::VertexFormatDesc GetVertexFormat()
	{
		Graphics::VertexFormatDesc desc = Graphics::VertexFormatDesc::FromAttribSet(
			Graphics::ATTRIB_POSITION | Graphics::ATTRIB_NORMAL | Graphics::ATTRIB_DIFFUSE | Graphics::ATTRIB_UV0);
		desc.attribs[4] = { Graphics::ATTRIB_FORMAT_UBYTE4, 10, 0, uint16_t(offsetof(VBOVertex, materials)) };
		desc.attribs[5] = { Graphics::ATTRIB_FORMAT_UBYTE4, 11, 0, uint16_t(offsetof(VBOVertex, materials) + 4) };
		desc.bindings[0].stride = sizeof(VBOVertex);
		return desc;
	}

	GeoPatchContext(const int _edgeLen)
	{
		m_edgeLen = _edgeLen + 2; // +2 for the skirt
		Init();
	}

	static void Refresh()
	{
		Init();
	}

	static void Init();

	static inline Graphics::IndexBuffer *GetIndexBuffer() { return m_indices.Get(); }

	static inline int NUMVERTICES() { return m_edgeLen * m_edgeLen; }

	static inline int GetEdgeLen() { return m_edgeLen; }
	static inline int GetNumTris() { return m_numTris; }
	static inline double GetFrac() { return m_frac; }

private:
	static int m_edgeLen;
	static int m_numTris;

	static double m_frac;

	static inline int VBO_COUNT_HI_EDGE() { return 3 * (m_edgeLen - 1); }
	static inline int VBO_COUNT_MID_IDX() { return (4 * 3 * (m_edgeLen - 3)) + 2 * (m_edgeLen - 3) * (m_edgeLen - 3) * 3; }
	//                                            ^^ serrated teeth bit  ^^^ square inner bit

	static inline int IDX_VBO_LO_OFFSET(const int i) { return i * sizeof(Uint32) * 3 * (m_edgeLen / 2); }
	static inline int IDX_VBO_HI_OFFSET(const int i) { return (i * sizeof(Uint32) * VBO_COUNT_HI_EDGE()) + IDX_VBO_LO_OFFSET(4); }

	static RefCountedPtr<Graphics::IndexBuffer> m_indices;
	static int m_prevEdgeLen;

	static void GenerateIndices();
};

#endif /* _GEOPATCHCONTEXT_H */
