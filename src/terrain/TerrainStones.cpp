// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "TerrainStones.h"

#include "FileSystem.h"
#include "core/Log.h"
#include "graphics/Renderer.h"
#include "graphics/Texture.h"
#include "vector2.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

// Everything here copies terrain-materials.glsl (terrainStonePick, terrainStoneAt, stoneSample, stoneTileHeight,
// stoneVariedRead, materialRegionNoise, materialCellHash, terrainSlotWeights, materialPlanes, materialStoneDepth) and
// must change with it. Single precision throughout, as on the graphics card.

namespace {
	// texture layers (TerrainMaterialTable::Layer, MATERIAL_* in terrain-materials.glsl)
	enum Layer {
		ROCK = 0, GRAVEL, SAND, SOIL, GRASS, FOREST, SNOW, ICE, BASALT, LAVA, ASH, REGOLITH, SALT, TUNDRA, DIRTY_ICE,
		LAVA_FLOW, CRACKED_MUD, DUST, METAL, FOREST_FAR, LAYERS
	};
	// in the order of the layers, as GeoSphere::SetUpMaterials loads them
	const char *const LAYER_FILES[LAYERS] = {
		"rock", "gravel", "sand", "soil", "grass", "forest", "snow", "ice", "basalt", "lava", "ash", "regolith", "salt",
		"tundra", "dirty_ice", "lava_flow", "cracked_mud", "dust", "metal", "forest_far"
	};

	// as in terrain-materials.glsl
	constexpr float STONE_HEIGHTS_SIZE = 1024.0f;
	constexpr int HEIGHTS_SIZE = 1024;
	constexpr int HEIGHTS_LEVELS = 11; // 1024 down to 1
	constexpr int DDS_SKIPPED_LEVELS = 2; // the files' 4096 and 2048 sizes
	constexpr float MATERIAL_MAX_LEVEL = 17.0f;
	constexpr float MATERIAL_ROCK_SLOPE_START = 0.12f;
	constexpr float MATERIAL_ROCK_SLOPE_FULL = 0.30f;
	constexpr float MATERIAL_BEACH_TOP = 8.0f;
	constexpr float MATERIAL_BEACH_FULL = 2.0f;
	constexpr float MATERIAL_BLEND_DEPTH = 0.2f;
	constexpr float MATERIAL_ICE_LEAST = 0.4f;
	constexpr float STONE_FLAT_MARGIN = 10.0f;
	// terrain-stone-aim.glsl
	const vector2f MATERIAL_TURNS[8] = { { 1.0f, 0.0f }, { 0.6f, 0.8f }, { 0.0f, 1.0f }, { -0.8f, 0.6f }, { -1.0f, 0.0f },
		{ -0.6f, -0.8f }, { 0.0f, -1.0f }, { 0.8f, -0.6f } };

	struct Heights {
		bool tried = false;
		bool loaded = false;
		// each layer's levels one after another, 1024 x 1024 first
		std::vector<uint8_t> layer[LAYERS];
		size_t levelOffset[HEIGHTS_LEVELS] = {};
		// each layer's turns (terrain-material-rotation.glsl): x turns, y patch size, z blend
		vector3f rotation[LAYERS];
	};
	Heights s_heights;
	RefCountedPtr<Graphics::Texture> s_texture;

	// ---------------------------------------------------------------- loading

	// the alpha (height) of a DXT5 level, unpacked
	void UnpackDXT5Alpha(const uint8_t *blocks, int width, int height, uint8_t *out)
	{
		const int blocksX = std::max((width + 3) / 4, 1), blocksY = std::max((height + 3) / 4, 1);
		for (int by = 0; by < blocksY; by++) {
			for (int bx = 0; bx < blocksX; bx++) {
				const uint8_t *b = blocks + (by * blocksX + bx) * 16;
				const int a0 = b[0], a1 = b[1];
				int palette[8];
				palette[0] = a0;
				palette[1] = a1;
				if (a0 > a1) {
					for (int i = 1; i <= 6; i++)
						palette[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
				} else {
					for (int i = 1; i <= 4; i++)
						palette[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
					palette[6] = 0;
					palette[7] = 255;
				}
				uint64_t bits = 0;
				for (int i = 0; i < 6; i++)
					bits |= uint64_t(b[2 + i]) << (8 * i);
				for (int t = 0; t < 16; t++) {
					const int x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
					if (x < width && y < height)
						out[y * width + x] = uint8_t(palette[(bits >> (3 * t)) & 7]);
				}
			}
		}
	}

	bool LoadLayer(int layer, std::vector<uint8_t> &out)
	{
		const std::string path = std::string("textures/terrain/") + LAYER_FILES[layer] + ".dds";
		RefCountedPtr<FileSystem::FileData> file = FileSystem::gameDataFiles.ReadFile(path);
		if (!file || file->GetSize() < 128)
			return false;
		const uint8_t *data = reinterpret_cast<const uint8_t *>(file->GetData());
		uint32_t height, width, mips;
		std::memcpy(&height, data + 12, 4);
		std::memcpy(&width, data + 16, 4);
		std::memcpy(&mips, data + 28, 4);
		if (std::memcmp(data, "DDS ", 4) != 0 || std::memcmp(data + 84, "DXT5", 4) != 0 ||
			width != uint32_t(HEIGHTS_SIZE << DDS_SKIPPED_LEVELS) || height != width || mips < uint32_t(DDS_SKIPPED_LEVELS + HEIGHTS_LEVELS))
			return false;
		size_t offset = 128;
		int w = int(width), h = int(height);
		for (int level = 0; level < DDS_SKIPPED_LEVELS + HEIGHTS_LEVELS; level++) {
			const size_t bytes = size_t(std::max((w + 3) / 4, 1)) * size_t(std::max((h + 3) / 4, 1)) * 16;
			if (offset + bytes > file->GetSize())
				return false;
			if (level >= DDS_SKIPPED_LEVELS) {
				const size_t at = s_heights.levelOffset[level - DDS_SKIPPED_LEVELS];
				UnpackDXT5Alpha(data + offset, w, h, out.data() + at);
			}
			offset += bytes;
			w = std::max(w / 2, 1);
			h = std::max(h / 2, 1);
		}
		return true;
	}

	// each layer's turns, from the lines terrain-material-rotation.glsl is made of:
	// "if (layer == N) return vec3(x, y, z);" and a last "return vec3(x, y, z);" for the rest
	void LoadRotations()
	{
		vector3f fallback(2.0f, 4.0f, 0.25f);
		bool given[LAYERS] = {};
		RefCountedPtr<FileSystem::FileData> file = FileSystem::gameDataFiles.ReadFile("shaders/opengl/terrain-material-rotation.glsl");
		if (file) {
			const std::string text(file->GetData(), file->GetSize());
			size_t start = 0;
			while (start < text.size()) {
				size_t end = text.find('\n', start);
				if (end == std::string::npos)
					end = text.size();
				const std::string line = text.substr(start, end - start);
				start = end + 1;
				int layer;
				float x, y, z;
				if (std::sscanf(line.c_str(), " if (layer == %d) return vec3(%f, %f, %f);", &layer, &x, &y, &z) == 4) {
					if (layer >= 0 && layer < LAYERS) {
						s_heights.rotation[layer] = vector3f(x, y, z);
						given[layer] = true;
					}
				} else if (std::sscanf(line.c_str(), " return vec3(%f, %f, %f);", &x, &y, &z) == 3) {
					fallback = vector3f(x, y, z);
				}
			}
		} else {
			Log::Warning("TerrainStones: can't read terrain-material-rotation.glsl; the stones' turns may not match the ground\n");
		}
		for (int i = 0; i < LAYERS; i++) {
			if (!given[i])
				s_heights.rotation[i] = fallback;
		}
	}

	void Load()
	{
		if (s_heights.tried)
			return;
		s_heights.tried = true;
		size_t total = 0;
		for (int level = 0; level < HEIGHTS_LEVELS; level++) {
			s_heights.levelOffset[level] = total;
			const size_t side = size_t(std::max(HEIGHTS_SIZE >> level, 1));
			total += side * side;
		}
		bool all = true;
		for (int layer = 0; layer < LAYERS; layer++) {
			s_heights.layer[layer].assign(total, 128);
			if (!LoadLayer(layer, s_heights.layer[layer])) {
				all = false;
				std::fill(s_heights.layer[layer].begin(), s_heights.layer[layer].end(), uint8_t(128));
				Log::Warning("TerrainStones: can't read the heights of textures/terrain/{}.dds; that ground stays flat\n", LAYER_FILES[layer]);
			}
		}
		LoadRotations();
		s_heights.loaded = all;
	}

	// ---------------------------------------------------------------- the shader's arithmetic

	inline float Clamp01(float x) { return std::min(std::max(x, 0.0f), 1.0f); }
	inline float SmoothStep(float e0, float e1, float x)
	{
		const float t = Clamp01((x - e0) / (e1 - e0));
		return t * t * (3.0f - 2.0f * t);
	}
	inline float Mix(float a, float b, float t) { return a * (1.0f - t) + b * t; }
	inline float Fract(float x) { return x - std::floor(x); }
	inline float Mod(float x, float y) { return x - y * std::floor(x / y); }

	float CellHashOf(float cx, float cy, uint32_t salt)
	{
		uint32_t x = uint32_t(int32_t(cx)), y = uint32_t(int32_t(cy)), z = salt;
		x = x * 1664525u + 1013904223u;
		y = y * 1664525u + 1013904223u;
		z = z * 1664525u + 1013904223u;
		x += y * z;
		y += z * x;
		z += x * y;
		x ^= x >> 16u;
		y ^= y >> 16u;
		z ^= z >> 16u;
		x += y * z;
		return float(x >> 8u) * (1.0f / 16777216.0f);
	}

	float RegionNoise(vector2f p, uint32_t salt)
	{
		float cx = std::floor(p.x), cy = std::floor(p.y);
		float tx = p.x - cx, ty = p.y - cy;
		tx = tx * tx * (3.0f - 2.0f * tx);
		ty = ty * ty * (3.0f - 2.0f * ty);
		const float nx = Mod(cx + 1.0f, 5.0f), ny = Mod(cy + 1.0f, 5.0f);
		cx = Mod(cx, 5.0f);
		cy = Mod(cy, 5.0f);
		const float h0 = CellHashOf(cx, cy, salt), h1 = CellHashOf(nx, cy, salt), h2 = CellHashOf(cx, ny, salt), h3 = CellHashOf(nx, ny, salt);
		return Mix(Mix(h0, h1, tx), Mix(h2, h3, tx), ty);
	}

	// textureLod() on the heights: trilinear, wrapping
	float Bilinear(int layer, int level, float u, float v)
	{
		const int size = std::max(HEIGHTS_SIZE >> level, 1);
		const uint8_t *texels = s_heights.layer[layer].data() + s_heights.levelOffset[level];
		const float x = u * float(size) - 0.5f, y = v * float(size) - 0.5f;
		const float x0 = std::floor(x), y0 = std::floor(y);
		const float fx = x - x0, fy = y - y0;
		auto wrap = [size](float i) {
			const int k = int(i) % size;
			return k < 0 ? k + size : k;
		};
		const int ix0 = wrap(x0), ix1 = wrap(x0 + 1.0f), iy0 = wrap(y0), iy1 = wrap(y0 + 1.0f);
		const float a = texels[iy0 * size + ix0], b = texels[iy0 * size + ix1];
		const float c = texels[iy1 * size + ix0], d = texels[iy1 * size + ix1];
		return Mix(Mix(a, b, fx), Mix(c, d, fx), fy) * (1.0f / 255.0f);
	}

	float StoneRead(int layer, vector2f uv, float lod)
	{
		lod = std::min(std::max(lod, 0.0f), float(HEIGHTS_LEVELS - 1));
		const int level = int(std::floor(lod));
		const float f = lod - float(level);
		const float a = Bilinear(layer, level, uv.x, uv.y);
		if (f <= 0.0f || level >= HEIGHTS_LEVELS - 1)
			return a;
		return Mix(a, Bilinear(layer, level + 1, uv.x, uv.y), f);
	}

	inline vector2f Turn(vector2f a, vector2f uv) { return vector2f(a.x * uv.x - a.y * uv.y, a.y * uv.x + a.x * uv.y); }

	float StoneVariedRead(int layer, vector2f uv, vector2f uvPatch, vector2f uvFar, int turnStep, float blend, float lod)
	{
		const float count = float(8 / turnStep);
		const float field = count * Fract(RegionNoise(uvPatch, 0u) + 3.0f * RegionNoise(uvFar, 1u));
		const float index = std::floor(field);
		const vector2f a = MATERIAL_TURNS[(int(index) * turnStep) & 7], b = MATERIAL_TURNS[(int(Mod(index + 1.0f, count)) * turnStep) & 7];
		const vector2f offsetA(Fract(0.7548777f * index), Fract(0.5698403f * index));
		const vector2f offsetB(Fract(0.7548777f * (index + 1.0f)), Fract(0.5698403f * (index + 1.0f)));
		const float ra = StoneRead(layer, Turn(a, uv) + offsetA, lod);
		const float rb = StoneRead(layer, Turn(b, uv) + offsetB, lod);
		return Mix(ra, rb, SmoothStep(0.5f - blend, 0.5f + blend, field - index + 0.8f * (rb - ra)));
	}

	// a point's mapping: where it is in planet-fixed metres (double, so the wrap at 5 tiles comes out exactly), and the
	// cube faces it's projected on
	struct Mapping {
		vector3d position;
		float plane[2];
		int axes[2];
	};

	// materialTileCoord(): the point in tiles of 2^level metres, wrapping at 5 tiles (the card's coordinates differ by a
	// whole number of wraps, which nothing it reads can tell)
	vector3f TileCoord(const Mapping &m, float level)
	{
		const double tile = std::exp2(double(level));
		auto wrap = [tile](double x) {
			const double q = std::fmod(x / tile, 5.0);
			return float(q < 0.0 ? q + 5.0 : q);
		};
		return vector3f(wrap(m.position.x), wrap(m.position.y), wrap(m.position.z));
	}

	inline vector2f PlaneUV(const vector3f &q, int axis)
	{
		return axis == 0 ? vector2f(q.y, q.z) : (axis == 1 ? vector2f(q.z, q.x) : vector2f(q.x, q.y));
	}

	float StoneTileHeight(const TerrainStones::Params &P, int layer, const Mapping &m, float level, float spacing)
	{
		const float tile = std::exp2(level);
		const vector3f q = TileCoord(m, level);
		const float lod = std::max(std::log2(std::max(spacing / tile * STONE_HEIGHTS_SIZE, 1e-6f)), 0.0f);
		const vector3f rotation = s_heights.rotation[layer];
		if (P.fixedDetail && rotation.x > 0.5f) {
			const vector3f qPatch = rotation.y < -0.5f ? q * 2.0f :
				(rotation.y < 0.5f ? q : TileCoord(m, std::min(level + rotation.y, MATERIAL_MAX_LEVEL)));
			const vector3f qFar = TileCoord(m, std::min(level + std::max(rotation.y, 0.0f) + 5.0f, MATERIAL_MAX_LEVEL));
			const int turnStep = rotation.x < 1.5f ? 2 : 1;
			float h = m.plane[0] * StoneVariedRead(layer, PlaneUV(q, m.axes[0]), PlaneUV(qPatch, m.axes[0]), PlaneUV(qFar, m.axes[0]), turnStep, rotation.z, lod);
			if (m.plane[1] > 0.0f)
				h += m.plane[1] * StoneVariedRead(layer, PlaneUV(q, m.axes[1]), PlaneUV(qPatch, m.axes[1]), PlaneUV(qFar, m.axes[1]), turnStep, rotation.z, lod);
			return h;
		}
		float h = m.plane[0] * StoneRead(layer, PlaneUV(q, m.axes[0]), lod);
		if (m.plane[1] > 0.0f)
			h += m.plane[1] * StoneRead(layer, PlaneUV(q, m.axes[1]), lod);
		return h;
	}

	float StoneSample(const TerrainStones::Params &P, int layer, const Mapping &m, float level, float spacing)
	{
		level = std::min(std::max(level, 0.0f), MATERIAL_MAX_LEVEL - 1.0f);
		const float k = std::floor(level);
		float h = StoneTileHeight(P, layer, m, k, spacing);
		if (level > k)
			h = Mix(h, StoneTileHeight(P, layer, m, k + 1.0f, spacing), level - k);
		if (layer == ICE)
			h = 0.5f + (h - 0.5f) * std::max(P.iceCracks, MATERIAL_ICE_LEAST);
		return h;
	}

	float StoneDepth(int m)
	{
		if (m == ROCK) return 0.10f;
		if (m == BASALT || m == LAVA_FLOW) return 0.08f;
		if (m == GRAVEL) return 0.05f;
		if (m == DIRTY_ICE || m == LAVA) return 0.04f;
		if (m == REGOLITH) return 0.03f;
		if (m == SOIL || m == CRACKED_MUD || m == TUNDRA || m == FOREST) return 0.025f;
		if (m == ICE || m == SNOW || m == ASH || m == GRASS) return 0.015f;
		if (m == SAND || m == DUST || m == SALT) return 0.008f;
		return 0.0f;
	}

	inline int SlotLayer(const TerrainStones::Params &P, int slot) { return int(P.slotLayer[slot]); }

	// terrain-stone-aim.glsl: terrainStonePixels(), terrainStoneAim()
	float StonePixels(const TerrainStones::Params &P, float dist)
	{
		const float tallest = P.deepest * P.stonesHeight * std::exp2(P.nearLevel) * (1.0f - SmoothStep(P.nearRange, 2.0f * P.nearRange, dist));
		return tallest * P.pixelsPerRadian / std::max(dist, 1e-3f);
	}

	float StoneAim(const TerrainStones::Params &P, float dist)
	{
		return std::max(P.aimPixels, 1.0f) * std::max(1.0f, std::max(P.fullPixels, 1.0f) / std::max(StonePixels(P, dist), 1e-3f));
	}

	void SlotWeights(const TerrainStones::Params &P, const float slots[16], float slope, float altitude, bool hasWater, float w[16])
	{
		for (int i = 0; i < 16; ++i)
			w[i] = slots[i];
		const float steep = SmoothStep(MATERIAL_ROCK_SLOPE_START, MATERIAL_ROCK_SLOPE_FULL, slope);
		if (P.rockSlot >= 0 && steep > 0.0f) {
			float moved = 0.0f;
			for (int i = 0; i < P.usedSlots; ++i) {
				if (i == P.rockSlot || i == P.lavaSlot)
					continue;
				const float k = (i == P.snowSlot) ? 0.7f * steep : steep;
				moved += k * w[i];
				w[i] *= 1.0f - k;
			}
			w[P.rockSlot] += moved;
		}
		if (hasWater && P.beachSlot >= 0) {
			const float beach = SmoothStep(MATERIAL_BEACH_TOP, MATERIAL_BEACH_FULL, altitude) * (1.0f - steep);
			if (beach > 0.0f) {
				float moved = 0.0f;
				for (int i = 0; i < P.usedSlots; ++i) {
					const int layer = SlotLayer(P, i);
					if (i == P.beachSlot || i == P.rockSlot || i == P.snowSlot || i == P.lavaSlot || layer == ASH || layer == LAVA_FLOW)
						continue;
					moved += beach * w[i];
					w[i] *= 1.0f - beach;
				}
				w[P.beachSlot] += moved;
			}
		}
	}
} // namespace

namespace TerrainStones {
	float CellHash(float cx, float cy, uint32_t salt)
	{
		return CellHashOf(cx, cy, salt);
	}

	float Depth(int layer)
	{
		return StoneDepth(layer);
	}

	bool Loaded()
	{
		return s_heights.loaded;
	}

	Graphics::Texture *GetTexture(Graphics::Renderer *renderer)
	{
		if (!s_texture) {
			Load();
			const Graphics::TextureDescriptor desc(Graphics::TEXTURE_R8, vector3f(float(HEIGHTS_SIZE), float(HEIGHTS_SIZE), float(LAYERS)),
				Graphics::LINEAR_REPEAT, true, false, false, HEIGHTS_LEVELS, Graphics::TEXTURE_2D_ARRAY);
			s_texture.Reset(renderer->CreateTexture(desc));
			Graphics::Texture::vecDataPtr layers;
			for (int layer = 0; layer < LAYERS; layer++)
				layers.push_back(s_heights.layer[layer].data());
			s_texture->Update(layers, desc.dataSize, Graphics::TEXTURE_R8, HEIGHTS_LEVELS);
		}
		return s_texture.Get();
	}

	double Lift(const Params &P, const Point &pt)
	{
		// terrainStonePick()
		if (!P.stonesOn || !P.nearOn || P.usedSlots <= 0)
			return 0.0;
		Load();
		const vector3f rel(pt.position - pt.camera);
		const float distance = rel.Length();
		float near = 1.0f - SmoothStep(P.nearRange, 2.0f * P.nearRange, distance);
		const float fadePixels = std::max(P.fadePixels, 0.1f);
		near *= SmoothStep(0.5f * fadePixels, fadePixels, StonePixels(P, distance));
		for (int i = 0; i < std::min(P.flatCount, int(Params::MAX_FLATS)); i++) {
			const float r = float(P.flatRadius[i]);
			if (r > 0.0f)
				near *= SmoothStep(r, r + STONE_FLAT_MARGIN, vector3f(pt.position - P.flatCentre[i]).Length());
		}
		if (pt.hasWater)
			near *= SmoothStep(0.5f, 2.0f, pt.altitude);
		if (near <= 0.0f)
			return 0.0;
		const vector3f up(pt.position.Normalized());
		const vector3f normal(pt.normal);
		float w[16];
		SlotWeights(P, pt.slots, 1.0f - normal.Dot(up), pt.altitude, pt.hasWater, w);
		const int usedSlots = std::min(P.usedSlots, 16);
		int m1 = 0, m2 = 1;
		for (int i = 1; i < usedSlots; ++i) {
			if (w[i] > w[m1]) {
				m2 = m1;
				m1 = i;
			} else if (i != m1 && w[i] > w[m2]) {
				m2 = i;
			}
		}
		float landWeight = 0.0f;
		for (int i = 0; i < usedSlots; ++i)
			landWeight += w[i];
		const int l1 = SlotLayer(P, m1), l2 = usedSlots > 1 ? SlotLayer(P, m2) : -1;
		if (landWeight < 1e-3f || l1 < 0 || l1 >= LAYERS || l2 >= LAYERS)
			return 0.0;
		const float w1 = w[m1], w2 = l2 >= 0 ? w[m2] : 0.0f;
		const float total = w1 + w2;
		const float scale = near * P.stonesHeight * std::exp2(P.nearLevel) / landWeight;
		const float k1 = scale * StoneDepth(l1), k2 = l2 >= 0 ? scale * StoneDepth(l2) : 0.0f;
		if (!(k1 > 0.0f || k2 > 0.0f))
			return 0.0;

		// terrainStoneMapping(), materialPlanes()
		Mapping m;
		m.position = pt.position;
		const vector3f a(std::abs(up.x), std::abs(up.y), std::abs(up.z));
		const int first = (a.x > a.y && a.x > a.z) ? 0 : (a.y > a.z ? 1 : 2);
		float rest[3] = { a.x, a.y, a.z };
		rest[first] = -1.0f;
		const int second = (rest[0] > rest[1] && rest[0] > rest[2]) ? 0 : (rest[1] > rest[2] ? 1 : 2);
		const float av[3] = { a.x, a.y, a.z };
		const float edge = SmoothStep(0.85f, 1.0f, av[second] / av[first]);
		m.axes[0] = first;
		m.axes[1] = second;
		m.plane[0] = 1.0f - 0.5f * edge;
		m.plane[1] = 0.5f * edge;

		// terrainStones(): the cut points' spacing; terrainStoneAt()
		const float spacing = std::max(distance * StoneAim(P, distance) / std::max(P.pixelsPerRadian, 1.0f), 1e-3f);
		const float h1 = StoneSample(P, l1, m, P.nearLevel, spacing);
		const float h2 = w2 > 0.01f ? StoneSample(P, l2, m, P.nearLevel, spacing) : 0.5f;
		const float s1 = w1 / std::max(total, 1e-6f) + h1;
		const float s2 = w2 / std::max(total, 1e-6f) + h2;
		const float top = std::max(s1, s2) - MATERIAL_BLEND_DEPTH;
		const float b1 = std::max(s1 - top, 0.0f), b2 = std::max(s2 - top, 0.0f);
		const float n1 = total * b1 / (b1 + b2), n2 = total * b2 / (b1 + b2);
		return double(n1 * h1 * k1 + n2 * h2 * k2);
	}
} // namespace TerrainStones
