// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAIN_H
#define _TERRAIN_H

#include "FracDef.h"
#include "SurfaceProfile.h"
#include "TerrainLandforms.h"
#include "TerrainMaterials.h"
#include "../Random.h"
#include "../RefCounted.h"
#include "../vector3.h"
#include "../Quaternion.h"
#include "../galaxy/SystemPath.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4250) // workaround for MSVC 2008 multiple inheritance bug
#endif

class SystemBody;
class TerrainErosion;

template <typename, typename>
class TerrainGenerator;

class Terrain : public RefCounted {
public:
	// location and intensity of effects are controlled by the colour fractals;
	// it's possible for a Terrain to have a flag set but not actually to exhibit any of that effect
	enum SurfaceEffectFlags {
		EFFECT_LAVA = 1 << 0,
		EFFECT_WATER = 2
		// can add other effect flags here (e.g., water, snow, ice)
	};

	static Terrain *InstanceTerrain(const SystemBody *body);

	// Builds the terrains of these bodies side by side, on as many threads as the processor has, for
	// PrebuiltOrNew to hand out: a system's bodies are made one after another, and each waited for its
	// own. The terrains are the same as InstanceTerrain makes. Any left from before are dropped first.
	static void Prebuild(const std::vector<const SystemBody *> &bodies);
	// the body's terrain from Prebuild, taken, or else a new one from InstanceTerrain
	static Terrain *PrebuiltOrNew(const SystemBody *body);
	// drops the terrains from Prebuild no body took
	static void DiscardPrebuilt();

	// 1: terrestrial bodies without a height map use the physical surface
	// generator (TerrainPhysical); 0: the classic height and colour fractals.
	// Bodies made after the call follow it.
	static void SetGenerator(int generator);

	virtual ~Terrain();

	void SetFracDef(const unsigned int index, const double featureHeightMeters, const double featureWidthMeters, const double smallestOctaveMeters = 20.0);
	inline const fracdef_t &GetFracDef(const unsigned int index) const
	{
		assert(index < MAX_FRACDEFS);
		return m_fracdef[index];
	}

	// Heights (unit-sphere units above the radius) at points on the unit
	// sphere: the height fractal, with river valleys cut in on bodies with
	// erosion (TerrainErosion), the landforms when they are switched on as
	// shape (AddLandforms), and the ground levelled under surface starports.
	// Collision and the terrain patches use this.
	void GetHeights(const vector3d *vP, double *heightsOut, const size_t count) const;
	// The height fractal alone, without erosion or levelling.
	virtual void GetBaseHeights(const vector3d *vP, double *heightsOut, const size_t count) const = 0;
	// Keeps the ground around p (unit sphere) as GetBaseHeights makes it,
	// for a surface starport's city, and levels the ground the starport
	// stands on, replacing any earlier site of the same starport. Call before
	// the terrain is drawn there.
	void ProtectSite(const SystemBody *starport, const vector3d &p);
	virtual vector3d GetColor(const vector3d &p, double height, const vector3d &norm) const = 0;
	// The colour and material weights of a point, worked out together. By default
	// the colour fractal's colour, with the weights read back from it.
	virtual void GetSurface(const vector3d &p, double height, const vector3d &normal, vector3d &colourOut, uint8_t weightsOut[TerrainMaterialTable::SLOT_COUNT]) const;

	virtual const char *GetHeightFractalName() const = 0;
	virtual const char *GetColorFractalName() const = 0;

	double GetMaxHeight() const { return m_maxHeight; }

	const SurfaceProfile &GetSurfaceProfile() const { return m_surfaceProfile; }

	// which materials the colour fractal paints, and with which colours;
	// empty for stars and gas giants
	const TerrainMaterialTable &GetMaterialTable() const { return m_materials; }
	// how strongly the ICE material shows its cracks, 0..1
	virtual float GetIceCrackStrength() const { return 0.7f; }
	// The landforms of the GPU detail tiles (rolling, ridged, dunes, craters).
	// False: go by the height fractal's name, as classic bodies do.
	virtual bool GetTileStyle(float[4]) const { return false; }
	// GetTileStyle, or else the style by the height fractal's name: craters on airless and cratered bodies, dunes on
	// dune terrains, ridges on mountains and rugged terrain, rolling eroded hills elsewhere
	void GetLandformStyle(float style[4]) const;
	// The landforms as real shape (P3 of docs/implementation_plans/opengl46-tessellation-compute_plan.md): their
	// settings for this body, from the body alone, never from the planet detail setting (TerrainLandforms)
	const TerrainLandforms::Params &GetLandformParams() const;
	// Being tried: the landforms in the heights, of every size up to this many metres (0: off, as before). From the
	// config (TerrainLandformShape) and the debug window; the patches must be made again after a change.
	static void SetLandformShape(double topMetres);
	static double GetLandformShape();
	// With it: only the landforms from this size up (metres) become shape, the large-scale shape; the smaller ones stay
	// painted by the GPU detail tiles, as with the shape off (2026-11-10). From the config (TerrainLandformShapeFrom)
	// and the debug window; the patches must be made again after a change.
	static void SetLandformShapeFrom(double metres);
	static double GetLandformShapeFrom();
	// The sizes the settings window's mod tab switches on ("Large landforms", on by default since 2026-11-10); others
	// only in the debug window. SetLargeLandforms: those, or none; the patches must be made again after a change.
	static constexpr double LARGE_LANDFORMS_FROM_M = 500.0;
	static constexpr double LARGE_LANDFORMS_TO_M = 40000.0;
	static void SetLargeLandforms(bool on);
	// How much the landforms made into shape darken their hollows and pale their crests in the ground's colour, as
	// craters do theirs (LandformShade): 0 none, 0.75 at most 75 % on bare airless ground (set in game on Mercury,
	// 2026-11-10). From the config (TerrainLandformShade); the patches must be made again after a change.
	static constexpr double LANDFORM_SHADE = 0.75;
	static void SetLandformShadeStrength(double strength);
	static double GetLandformShadeStrength();
	// How the landforms made into shape fade out from afar, where the samples grow too far apart to show them well, in
	// samples across a hill: their height (crumpled ground from orbit otherwise), and their shade (speckled). From the
	// config (TerrainLandformFade*) and the debug window; the patches must be made again after a change. The shade comes
	// in slowly, all of it only close up (set in game on Mercury, 2026-11-10).
	static constexpr TerrainLandforms::Fade LANDFORM_HEIGHT_FADE = { 2.0, 8.0 };
	static constexpr TerrainLandforms::Fade LANDFORM_SHADE_FADE = { 2.0, 64.0 };
	static void SetLandformHeightFade(TerrainLandforms::Fade fade);
	static TerrainLandforms::Fade GetLandformHeightFade();
	static void SetLandformShadeFade(TerrainLandforms::Fade fade);
	static TerrainLandforms::Fade GetLandformShadeFade();
	// -1..1: how far the landforms AddLandforms makes into shape put p above or below the ground around it (low in
	// hollows, high on crests), as far as the samples of the current band limit show them; 0 while there are none
	double LandformShade(const vector3d &p) const;
	// 0..1: how much of the shade shows through this body's air, which lights the hollows from all round (1: none)
	virtual double LandformShadeAir() const { return 1.0; }
	// The largest craters the tiles may draw, in metres; 0: by the planet's size.
	virtual double GetTileCraterTop() const { return 0.0; }
	// metres: the tile's landforms, of every size up to this and fixed in space, instead of only those near the patch's
	// own scale (0)
	virtual double GetTileLandformTop() const { return 0.0; }
	// 0..1: how much of the tile's landforms are cracked slabs and blocks, as on young lava and bare rock
	virtual double GetTileBlocks() const { return 0.0; }
	// True: all detail is fixed in space, so the shaders leave out what they size by the view
	virtual bool HasFixedDetail() const { return false; }
	// how many times as steep the light takes slopes from afar (terrainFarRelief in terrain-far.glsl)
	virtual double GetFarReliefSlopes() const { return 4.0; }
	// Water that the height doesn't show, such as rivers and lakes above sea level, within radiusMetres of p
	virtual bool HasWaterAround(const vector3d &p, double radiusMetres) const { return false; }
	// what the Terrain Debug window shows of a body's geology
	virtual std::vector<std::string> GetGeologyInfo() const { return {}; }
	// starts the physical generator's timers again, for the debug window
	static void ResetSpeedCounters();
	// what they have counted, for the log. One call in 16 is timed, so a call count is of those.
	struct SpeedTotals {
		uint64_t pointCalls = 0, pointNanos = 0;
		uint64_t valleyCalls = 0, valleyNanos = 0;
		uint64_t paintCalls = 0, paintNanos = 0;
		uint64_t chunks = 0, chunkMicros = 0;
		uint64_t chunksAtLevel[9] = {}; // valley chunks, largest valleys first
	};
	static SpeedTotals GetSpeedTotals();
	// For the debug tools: the places of a kind of feature (river, fork, mouth, lake, coast, peak),
	// with a size in metres, and a description of the ground at a point
	struct Feature {
		vector3d position;
		double size;
	};
	// (small lakes are looked for only around a place on the unit sphere: kind smalllake)
	virtual std::vector<Feature> FindFeatures(const std::string &kind, const vector3d &around) const { return {}; }
	virtual std::string DescribePoint(const vector3d &p) const { return {}; }
	// For the debug tools: what the height at a point on the unit sphere is made of, in metres. False if this
	// terrain doesn't say. They don't add up to the total: some are scaled on the way, and more goes in.
	struct HeightLayers {
		double total = 0.0;
		double shape = 0.0; // plates, plains and the like, before craters
		double largeCraters = 0.0;
		double smallCraters = 0.0;
		double smallRelief = 0.0;
		double lines = 0.0; // scarps, cracks, valleys and bands
		double maps = 0.0; // the ground maps of icy shells and grooves
		double volcano = 0.0;
	};
	virtual bool GetHeightLayers(const vector3d &p, HeightLayers &out) const { return false; }
	// the craters behind a step in the height between two nearby points, as lines for the log
	virtual std::vector<std::string> DescribeCraterStep(const vector3d &a, const vector3d &b) const { return {}; }
	// and the tectonic lines around them
	virtual std::vector<std::string> DescribeLineStep(const vector3d &a, const vector3d &b) const { return {}; }

	Uint32 GetSurfaceEffects() const { return m_surfaceEffects; }

	double BiCubicInterpolation(const vector3d &p) const;

	// Signed altitude above sea level in metres for a point p on the unit
	// sphere with terrain height `height` (from GetHeights): negative under
	// water, where it is the water depth.
	virtual double GetSurfaceAltitude(const vector3d &p, const double height) const;

	void DebugDump() const;

private:
	template <typename HeightFractal, typename ColorFractal>
	static Terrain *InstanceGenerator(const SystemBody *body) { return new TerrainGenerator<HeightFractal, ColorFractal>(body); }

	typedef Terrain *(*GeneratorInstancer)(const SystemBody *);

protected:
	Terrain(const SystemBody *body);

	Uint32 m_seed;
	Random m_rand;

	double m_sealevel; // 0 - no water, 1 - 100% coverage
	double m_icyness; // 0 - 1 (0% to 100% cover)
	double m_volcanic;

	Uint32 m_surfaceEffects;

	// heightmap stuff
	// XXX unify heightmap types
	std::unique_ptr<double[]> m_heightMap;
	double m_heightScaling, m_minh;

	int m_heightMapSizeX;
	int m_heightMapSizeY;

	/** General attributes */
	double m_maxHeight;
	double m_maxHeightInMeters;
	double m_invMaxHeight;
	double m_planetRadius;
	double m_invPlanetRadius;
	double m_planetEarthRadii;

	double m_entropy;

	vector3d m_rockColor[8];
	vector3d m_darkrockColor[8];
	vector3d m_greyrockColor[8];
	vector3d m_plantColor[8];
	vector3d m_darkplantColor[8];
	vector3d m_sandColor[8];
	vector3d m_darksandColor[8];
	vector3d m_dirtColor[8];
	vector3d m_darkdirtColor[8];
	vector3d m_gglightColor[8];
	vector3d m_ggdarkColor[8];

	/* XXX you probably shouldn't increase this. If you are
	   using more than 10 then things will be slow as hell */
	static const Uint32 MAX_FRACDEFS = 10;
	fracdef_t m_fracdef[MAX_FRACDEFS];
	Quaterniond m_quatDefs[MAX_FRACDEFS];

	// Colour noise, owned by the colour fractals so its feature sizes no longer
	// depend on which height fractal the body uses. Slots 0-8 take over the
	// m_fracdef slots the colour code used to read; COLOR_FRACDEF_CLIMATE
	// drives the regional "equatorial desert" variation.
	static const Uint32 MAX_COLOR_FRACDEFS = 10;
	static const Uint32 COLOR_FRACDEF_CLIMATE = 9;
	fracdef_t m_colorFracdef[MAX_COLOR_FRACDEFS];
	void SetColorFracDef(const unsigned int index, const double featureWidthMeters, const double smallestOctaveMeters);

	static double SmoothStep(const double edge0, const double edge1, const double x)
	{
		const double t = std::min(1.0, std::max(0.0, (x - edge0) / (edge1 - edge0)));
		return t * t * (3.0 - 2.0 * t);
	}

	// Blends altitude bands smoothly instead of switching at hard thresholds.
	// thresholds[] holds the lower bound of bands 0..count-1 in descending
	// order; band count covers everything below the last threshold. band(i, n)
	// returns the colour of band i at height n; near a threshold it is also
	// called for heights just outside band i.
	template <typename BandFn>
	static vector3d BlendBands(const double n, const double *thresholds, const int count, BandFn band)
	{
		int i = 0;
		while (i < count && n <= thresholds[i])
			++i;

		// blend across 30% of the narrower neighbouring band on each side
		auto halfWidth = [&](const int t) {
			const double above = (t > 0) ? thresholds[t - 1] - thresholds[t] : thresholds[t];
			const double below = (t + 1 < count) ? thresholds[t] - thresholds[t + 1] : thresholds[t];
			return 0.3 * std::min(above, below);
		};

		if (i < count) {
			const double w = halfWidth(i);
			if (n < thresholds[i] + w) {
				const double t = SmoothStep(thresholds[i] - w, thresholds[i] + w, n);
				return band(i + 1, n) * (1.0 - t) + band(i, n) * t;
			}
		}
		if (i > 0) {
			const double w = halfWidth(i - 1);
			if (n > thresholds[i - 1] - w) {
				const double t = SmoothStep(thresholds[i - 1] - w, thresholds[i - 1] + w, n);
				return band(i, n) * (1.0 - t) + band(i - 1, n) * t;
			}
		}
		return band(i, n);
	}

	struct MinBodyData {
		MinBodyData(const SystemBody *body);
		double m_radius;
		double m_radiusEarthRatio;
		double m_aspectRatio;
		SystemPath m_path;
		std::string m_name;
	};
	MinBodyData m_minBody;

	// surface parameters derived from the body; provides the rocky palettes
	SurfaceProfile m_surfaceProfile;

	// material tables, filled in by the colour fractals' constructors
	TerrainMaterialTable m_materials;

	// river valleys, null on bodies without erosion
	std::unique_ptr<TerrainErosion> m_erosion;

	// ground levelled under a surface starport: flat out to flatChord from
	// pos, back to the natural ground at blendChord (chords on the unit sphere)
	struct FlatSite {
		const SystemBody *starport;
		vector3d pos;
		double height; // unit-sphere units, the base height at pos
		double flatChord;
		double blendChord;
	};
	// replaced as a whole when a site changes, so height jobs on other
	// threads keep a consistent copy
	std::shared_ptr<const std::vector<FlatSite>> m_flatSites;
	mutable std::mutex m_flatSitesLock;
	std::shared_ptr<const std::vector<FlatSite>> FlatSites() const;
	// the site where the starport stands now (its orbit plane), set up once
	// the height fractal can be read
	void AddStarportSites(const SystemBody *body);
	void SetFlatSite(const SystemBody *starport, const vector3d &p);
	void LevelSites(const vector3d *vP, double *heightsOut, const size_t count) const;
	// adds the landforms to the heights, while they are switched on (SetLandformShape)
	void AddLandforms(const vector3d *vP, double *heightsOut, const size_t count) const;
	bool m_airless = false;
	mutable std::once_flag m_landformOnce;
	mutable TerrainLandforms::Params m_landformParams;

	// the palette materials of the Earth-like colour fractals (EarthLike, TFGood, TFPoor)
	void AddPaletteMaterials();
	// barren rock: regolith on the flats, basalt (airless) or gravel in the dark patches
	void AddBarrenMaterials(const SystemBody *body);
};

template <typename HeightFractal>
class TerrainHeightFractal : virtual public Terrain {
public:
	TerrainHeightFractal() = delete;
	void GetBaseHeights(const vector3d *vP, double *heightsOut, const size_t count) const final;
	const char *GetHeightFractalName() const final;

protected:
	TerrainHeightFractal(const SystemBody *body);
};

template <typename ColorFractal>
class TerrainColorFractal : virtual public Terrain {
public:
	TerrainColorFractal() = delete;
	vector3d GetColor(const vector3d &p, double height, const vector3d &norm) const final;
	const char *GetColorFractalName() const final;

protected:
	TerrainColorFractal(const SystemBody *body);
};

template <typename HeightFractal, typename ColorFractal>
class TerrainGenerator : public TerrainHeightFractal<HeightFractal>, public TerrainColorFractal<ColorFractal> {
public:
	TerrainGenerator() = delete;
	TerrainGenerator(const SystemBody *body) :
		Terrain(body),
		TerrainHeightFractal<HeightFractal>(body),
		TerrainColorFractal<ColorFractal>(body) {}

private:
};

//This is the most complex and insanely crazy terrain you will ever see :
class TerrainHeightFlat;

//New terrains with less noise :
class TerrainHeightAsteroid;
class TerrainHeightAsteroid2;
class TerrainHeightAsteroid3;
class TerrainHeightAsteroid4;
class TerrainHeightBarrenRock;
class TerrainHeightBarrenRock2;
class TerrainHeightBarrenRock3;
/* Pictures of the above terrains:
 http://i.imgur.com/cJO4E.jpg
 http://i.imgur.com/BtB0g.png
 http://i.imgur.com/qeEuS.png
 */

class TerrainHeightEllipsoid;

// Newish terrains, 6 months or so :
class TerrainHeightHillsCraters2;
class TerrainHeightHillsCraters;
class TerrainHeightHillsDunes;
//   This terrain or the following one should have terragen style ridged mountains :
//   (As seen in an ancient clip of Mars http://www.youtube.com/watch?v=WeO28VBTWxs )
class TerrainHeightHillsNormal;
class TerrainHeightHillsRidged;
class TerrainHeightHillsRivers;

class TerrainHeightMapped;
class TerrainHeightMapped2;
class TerrainHeightMountainsCraters2;
class TerrainHeightMountainsCraters;

//Probably the best looking terrain due to variety, but among the most costly too :
//(It was also used for mars at some point : http://www.youtube.com/watch?feature=player_embedded&v=4-DcyQm0zE4 )
/// and http://www.youtube.com/watch?v=gPtxUUunSWg&t=5m15s
class TerrainHeightMountainsNormal;
// Based on TerrainHeightMountainsNormal :
class TerrainHeightMountainsRivers;
/*Pictures from the above two terrains generating Earth-like worlds:
 http://www.spacesimcentral.com/forum/download/file.php?id=1533&mode=view
 http://www.spacesimcentral.com/forum/download/file.php?id=1544&mode=view
 http://www.spacesimcentral.com/forum/download/file.php?id=1550&mode=view
 http://www.spacesimcentral.com/forum/download/file.php?id=1540&mode=view
 */

// Older terrains:
class TerrainHeightMountainsRidged;
class TerrainHeightMountainsRiversVolcano;
//   Used to be used for mars since it has a megavolcano:
class TerrainHeightMountainsVolcano;

//Oldest terrains, from before fracdefs :
class TerrainHeightRuggedDesert;
//   lava terrain should look like this http://www.spacesimcentral.com/forum/download/file.php?id=1778&mode=view
class TerrainHeightRuggedLava;

/*Terrains used for Iceworlds,
only terrain to use the much neglected impact crater function
(basically I forgot about it;) ) **It makes cool looking sunken craters** */
class TerrainHeightWaterSolidCanyons;
class TerrainHeightWaterSolid;

class TerrainColorAsteroid;
class TerrainColorBandedRock;
class TerrainColorBlack;
class TerrainColorDesert;
/*ColorEarthlike uses features not yet included in all terrain colours
 such as better poles : http://www.spacesimcentral.com/forum/download/file.php?id=1884&mode=view
 http://www.spacesimcentral.com/forum/download/file.php?id=1885&mode=view
and better distribution of snow :  http://www.spacesimcentral.com/forum/download/file.php?id=1879&mode=view  */
class TerrainColorEarthLike;
class TerrainColorEarthLikeHeightmapped;
class TerrainColorGGJupiter;
class TerrainColorGGNeptune2;
class TerrainColorGGNeptune;
class TerrainColorGGSaturn2;
class TerrainColorGGSaturn;
class TerrainColorGGUranus;
class TerrainColorIce;
class TerrainColorMethane;
class TerrainColorRock2;
class TerrainColorRock;
class TerrainColorWhite;
class TerrainColorStarBrownDwarf;
class TerrainColorStarG;
class TerrainColorStarK;
class TerrainColorStarM;
class TerrainColorStarWhiteDwarf;
class TerrainColorTFGood;
class TerrainColorTFPoor;
class TerrainColorVolcanic;

#ifdef _MSC_VER
#pragma warning(default : 4250)
#endif

#endif /* TERRAIN_H */
