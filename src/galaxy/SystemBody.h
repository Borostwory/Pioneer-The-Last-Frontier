// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef SYSTEMBODY_H
#define SYSTEMBODY_H

#include "Color.h"
#include "IterationProxy.h"
#include "JsonFwd.h"
#include "Orbit.h"
#include "RefCounted.h"
#include "fixed.h"
#include "galaxy/AtmosphereProfile.h"
#include "galaxy/RingStyle.h"
#include "galaxy/SystemPath.h"
#include "gameconsts.h"

class StarSystem;

struct AtmosphereParameters;

// Enum scoped access pattern base class
// Allows access to e.g. SystemBody::TYPE_GRAVPOINT
class SystemBodyType {
public:
	enum BodyType : uint32_t { // <enum scope='SystemBodyType' prefix=TYPE_ public>
		TYPE_GRAVPOINT = 0,
		TYPE_BROWN_DWARF = 1, //  L+T Class Brown Dwarfs
		TYPE_WHITE_DWARF = 2,
		TYPE_STAR_M = 3, //red
		TYPE_STAR_K = 4, //orange
		TYPE_STAR_G = 5, //yellow
		TYPE_STAR_F = 6, //white
		TYPE_STAR_A = 7, //blue/white
		TYPE_STAR_B = 8, //blue
		TYPE_STAR_O = 9, //blue/purple/white
		TYPE_STAR_M_GIANT = 10,
		TYPE_STAR_K_GIANT = 11,
		TYPE_STAR_G_GIANT = 12,
		TYPE_STAR_F_GIANT = 13,
		TYPE_STAR_A_GIANT = 14,
		TYPE_STAR_B_GIANT = 15,
		TYPE_STAR_O_GIANT = 16,
		TYPE_STAR_M_SUPER_GIANT = 17,
		TYPE_STAR_K_SUPER_GIANT = 18,
		TYPE_STAR_G_SUPER_GIANT = 19,
		TYPE_STAR_F_SUPER_GIANT = 20,
		TYPE_STAR_A_SUPER_GIANT = 21,
		TYPE_STAR_B_SUPER_GIANT = 22,
		TYPE_STAR_O_SUPER_GIANT = 23,
		TYPE_STAR_M_HYPER_GIANT = 24,
		TYPE_STAR_K_HYPER_GIANT = 25,
		TYPE_STAR_G_HYPER_GIANT = 26,
		TYPE_STAR_F_HYPER_GIANT = 27,
		TYPE_STAR_A_HYPER_GIANT = 28,
		TYPE_STAR_B_HYPER_GIANT = 29,
		TYPE_STAR_O_HYPER_GIANT = 30, // these various stars do exist = they are transitional states and are rare
		TYPE_STAR_M_WF = 31,		  //Wolf-Rayet star
		TYPE_STAR_B_WF = 32,		  // while you do not specifically get class M,B or O WF stars,
		TYPE_STAR_O_WF = 33,		  //  you do get red = blue and purple from the colour of the gasses = so spectral class is an easy way to define them.
		TYPE_STAR_S_BH = 34,		  //stellar blackhole
		TYPE_STAR_IM_BH = 35,		  //Intermediate-mass blackhole
		TYPE_STAR_SM_BH = 36,		  //Supermassive blackhole
		TYPE_PLANET_GAS_GIANT = 37,
		TYPE_PLANET_ASTEROID = 38,
		TYPE_PLANET_TERRESTRIAL = 39,
		TYPE_STARPORT_ORBITAL = 40,
		TYPE_STARPORT_SURFACE = 41,
		TYPE_MIN = TYPE_BROWN_DWARF,	  // <enum skip>
		TYPE_MAX = TYPE_STARPORT_SURFACE, // <enum skip>
		TYPE_STAR_MIN = TYPE_BROWN_DWARF, // <enum skip>
		TYPE_STAR_MAX = TYPE_STAR_SM_BH,  // <enum skip>
										  // XXX need larger atmosphereless thing
	};

	enum BodySuperType : uint32_t { // <enum scope='SystemBodyType' prefix=SUPERTYPE_ public>
		SUPERTYPE_NONE = 0,
		SUPERTYPE_STAR = 1,
		SUPERTYPE_ROCKY_PLANET = 2,
		SUPERTYPE_GAS_GIANT = 3,
		SUPERTYPE_STARPORT = 4,
	};
};

/**
 * Base class containing only static data parameters for SystemBody
 *
 * All "runtime" data parameters are generated in SystemBody.cpp or StarSystemGenerator.cpp
 */
class SystemBodyData {
public:
	SystemBodyData();

	void SaveToJson(Json &out);
	void LoadFromJson(const Json &obj);
	void GenerateStarColor();

	std::string m_name;
	SystemBodyType::BodyType m_type = SystemBodyType::TYPE_GRAVPOINT;

	Uint32 m_seed;                  // Planet.cpp can use to generate terrain
	fixed m_radius;					// in earth radii for planets, sol radii for stars. equatorial radius in case of bodies which are flattened at the poles
	fixed m_aspectRatio;			// ratio between equatorial and polar radius for bodies with eqatorial bulges
	fixed m_mass;					// earth masses if planet, solar masses if star
	fixed m_rotationPeriod;			// in days
	fixed m_rotationalPhaseAtStart; // 0 to 2 pi
	fixed m_humanActivity;			// 0 - 1
	fixed m_semiMajorAxis;			// in AUs
	fixed m_eccentricity;           // 0 - 1
	fixed m_orbitalOffset;          // 0 to 2 pi in radians, counterclockwise (long. of ascending node)
	fixed m_orbitalPhaseAtStart;    // 0 to 2 pi in radians, counterclockwise (true anomaly at epoch)
	fixed m_axialTilt;			    // in radians
	fixed m_inclination;		    // in radians, for surface bodies = latitude
	fixed m_argOfPeriapsis;         // in radians, counterclockwise
	int m_averageTemp;              // surface temperature in degrees Kelvin

	/* composition */
	fixed m_metallicity;	// (crust) 0.0 = light (Al, SiO2, etc), 1.0 = heavy (Fe, heavy metals)
	fixed m_volcanicity;	// 0 = none, 1.0 = fucking volcanic
	fixed m_volatileLiquid; // 1.0 = 100% ocean cover (earth = 70%)
	fixed m_volatileIces;	// 1.0 = 100% ice cover (earth = 3%)
	fixed m_volatileGas;	// 1.225 = earth atmosphere density, kg/m^3
	// TODO: atmosphere hydrogen-nitrogen ratio from mass, carbon-oxygen ratio from atmosCarbon
	// (approximate the distribution of nitrogen vs. hydrogen by using the mass of the body; < 5 Me predominantly nitrogen, > 7 Me predominantly hydrogen)
	// this differentiates nitrogen-rich and hydrogen rich atmospheres, as well as (hydro)carbon-rich from oxygen-rich atmospheres
	// thus, an earth-like atmosphere would be atmosOxidizing = 0.3, atmosCarbon <= 0.05
	// See Photochemistry in Terrestrial Exoplanet Atmospheres[...] (http://dx.doi.org/10.1088/0004-637X/784/1/63)
	// particularly Fig. 5: https://www.researchgate.net/figure/Mixing-ratios-of-common-molecules-in-thick-atmospheres-on-a-GJ-1214-b-like-exoplanet-The_fig2_259578190
	fixed m_atmosOxidizing; // 0.0 = reducing (H2, NH3, etc), 1.0 = oxidising (CO2, O2, etc)
	// fixed m_atmosCarbon;    // 0.0 = oxygen (H2O, O2), 1.0 = carbon (CO, CO2, CH4, C2H4, etc)
	fixed m_life;			// 0.0 = dead, 1.0 = teeming

	/* economy type stuff */
	fixed m_population;
	fixed m_agricultural;

	RingStyle m_rings;
	Color m_atmosColor;

	// the air's main gas and haze set by hand (AtmosphereProfile names such as "co2" and
	// "acid_deck"); empty, and an amount below 0, leave them to SystemBody's rules
	std::string m_atmosGasName;
	std::string m_atmosHazeName;
	double m_atmosHazeAmount = -1.0;

	std::string m_heightMapFilename;
	unsigned int m_heightMapFractal;
	// what a height map's stored values become, in metres: value * scale + offset, for a map stored
	// compressed or from another datum
	double m_heightMapScale = 1.0;
	double m_heightMapOffset = 0.0;

	std::string m_spaceStationType;

	// star color
	Color m_starColor;
};

class SystemBody : public RefCounted, public SystemBodyType, protected SystemBodyData {
public:
	class EditorAPI; // Allows editor API limited access to data members without rewriting entire class

public:
	SystemBody(const SystemPath &path, StarSystem *system);

	const SystemPath &GetPath() const { return m_path; }
	SystemBody *GetParent() const { return m_parent; }

	bool IsPlanet() const;
	bool IsMoon() const { return GetSuperType() == SUPERTYPE_ROCKY_PLANET && !IsPlanet(); }
	// We allow hyperjump to any star of the system
	bool IsJumpable() const { return GetSuperType() == SUPERTYPE_STAR; }
	SystemBody *GetNearestJumpable(double atTime);

	bool HasChildren() const { return !m_children.empty(); }
	Uint32 GetNumChildren() const { return static_cast<Uint32>(m_children.size()); }
	IterationProxy<std::vector<SystemBody *>> GetChildren() { return MakeIterationProxy(m_children); }
	const IterationProxy<const std::vector<SystemBody *>> GetChildren() const { return MakeIterationProxy(m_children); }

	const std::vector<SystemBody *> CollectAllChildren();

	inline const std::string &GetName() const { return m_name; }
	std::string GetAstroDescription() const;
	const char *GetIcon() const;
	BodyType GetType() const { return BodyType(m_type); }
	BodySuperType GetSuperType() const;
	bool IsCustomBody() const { return m_isCustomBody; }
	bool IsCoOrbitalWith(const SystemBody *other) const; //this and other form a binary pair
	bool IsCoOrbital() const;							 //is part of any binary pair
	fixed GetRadiusAsFixed() const { return m_radius; }

	// the aspect ratio adjustment is converting from equatorial to polar radius to account for ellipsoid bodies, used for calculating terrains etc
	inline double GetRadius() const
	{
		if (GetSuperType() <= SUPERTYPE_STAR) // polar radius
			return (m_radius.ToDouble() / m_aspectRatio.ToDouble()) * SOL_RADIUS;
		else
			return m_radius.ToDouble() * EARTH_RADIUS;
	}

	// the un-adjusted equatorial radius is necessary for calculating the radius of frames, see Space.cpp `MakeFrameFor`
	inline double GetEquatorialRadius() const
	{
		if (GetSuperType() <= SUPERTYPE_STAR) // equatorial radius
			return m_radius.ToDouble() * SOL_RADIUS;
		else
			return m_radius.ToDouble() * EARTH_RADIUS;
	}

	double GetAspectRatio() const { return m_aspectRatio.ToDouble(); }
	fixed GetMassAsFixed() const { return m_mass; }
	double GetMass() const
	{
		if (GetSuperType() <= SUPERTYPE_STAR)
			return m_mass.ToDouble() * SOL_MASS;
		else
			return m_mass.ToDouble() * EARTH_MASS;
	}
	fixed GetMassInEarths() const
	{
		if (GetSuperType() <= SUPERTYPE_STAR)
			return m_mass * 332998;
		else
			return m_mass;
	}
	fixed GetMassInSols() const
	{
		if (GetSuperType() <= SUPERTYPE_STAR)
			return m_mass;
		else
			return m_mass / 332998;
	}
	bool IsRotating() const { return m_rotationPeriod != fixed(0); }
	// returned in seconds
	double GetRotationPeriodInDays() const { return m_rotationPeriod.ToDouble(); }
	fixed GetRotationPeriodAsFixed() const { return m_rotationPeriod; }
	double GetRotationPeriod() const
	{
		return m_rotationPeriod.ToDouble() * 60 * 60 * 24;
	}
	bool HasRotationPhase() const { return m_rotationalPhaseAtStart != fixed(0); }
	double GetRotationPhaseAtStart() const { return m_rotationalPhaseAtStart.ToDouble(); }
	double GetAxialTilt() const { return m_axialTilt.ToDouble(); }
	fixed GetAxialTiltAsFixed() const { return m_axialTilt; }

	const Orbit &GetOrbit() const { return m_orbit; }
	double GetEccentricity() const { return m_eccentricity.ToDouble(); }
	fixed GetEccentricityAsFixed() const { return m_eccentricity; }
	double GetOrbMin() const { return m_orbMin.ToDouble(); }
	double GetOrbMax() const { return m_orbMax.ToDouble(); }
	fixed GetOrbMinAsFixed() const { return m_orbMin; }
	fixed GetOrbMaxAsFixed() const { return m_orbMax; }
	double GetSemiMajorAxis() const { return m_semiMajorAxis.ToDouble(); }
	fixed GetSemiMajorAxisAsFixed() const { return m_semiMajorAxis; }
	fixed GetInclinationAsFixed() const { return m_inclination; }
	fixed GetArgOfPeriapsisAsFixed() const { return m_argOfPeriapsis; }
	void SetOrbitPlane(const matrix3x3d &orient) { m_orbit.SetPlane(orient); }

	int GetAverageTemp() const { return m_averageTemp; }
	inline const std::string &GetHeightMapFilename() const { return m_heightMapFilename; }
	unsigned int GetHeightMapFractal() const { return m_heightMapFractal; }
	double GetHeightMapScale() const { return m_heightMapScale; }
	double GetHeightMapOffset() const { return m_heightMapOffset; }

	Uint32 GetSeed() const { return m_seed; }

	fixed GetMetallicityAsFixed() const { return m_metallicity; }
	double GetMetallicity() const { return m_metallicity.ToDouble(); }
	fixed GetVolatileGasAsFixed() const { return m_volatileGas; }
	double GetVolatileGas() const { return m_volatileGas.ToDouble(); }
	fixed GetVolatileLiquidAsFixed() const { return m_volatileLiquid; }
	double GetVolatileLiquid() const { return m_volatileLiquid.ToDouble(); }
	fixed GetVolatileIcesAsFixed() const { return m_volatileIces; }
	double GetVolatileIces() const { return m_volatileIces.ToDouble(); }
	fixed GetVolcanicityAsFixed() const { return m_volcanicity; }
	double GetVolcanicity() const { return m_volcanicity.ToDouble(); }
	double GetAtmosOxidizing() const { return m_atmosOxidizing.ToDouble(); }
	fixed GetLifeAsFixed() const { return m_life; }
	double GetLife() const { return m_life.ToDouble(); }

	fixed GetAgriculturalAsFixed() const { return m_agricultural; }
	double GetAgricultural() const { return m_agricultural.ToDouble(); }
	fixed GetPopulationAsFixed() const { return m_population; }
	double GetPopulation() const { return m_population.ToDouble(); }

	double CalcSurfaceGravity() const;
	double CalcEscapeVelocity() const;
	double CalcMeanDensity() const;

	double GetMaxChildOrbitalDistance() const;

	bool HasRings() const { return bool(m_rings.maxRadius.v); }
	const RingStyle &GetRings() const { return m_rings; }

	// XXX merge all this atmosphere stuff
	bool HasAtmosphere() const;

	Color GetAlbedo() const
	{
		// XXX suggestions about how to determine a sensible albedo colour would be welcome
		// Currently (2014-03-24) this is just used as the colour for the body billboard
		// which is rendered when the body has a small screen size
		return Color(200, 200, 200, 255);
	}
	Color GetStarColor() const { return m_starColor; };

	// Returns color, density in kg/m^3 at sea level
	void GetAtmosphereFlavor(Color *outColor, double *outDensity) const
	{
		*outColor = m_atmosColor;
		*outDensity = m_volatileGas.ToDouble();
	}

	double GetAtmSurfaceDensity() const { return m_volatileGas.ToDouble(); }
	double GetAtmSurfacePressure() const { return m_atmosPressure; }
	double GetAtmRadius() const { return m_atmosRadius; }
	double GetTropopause() const { return m_tropopause; }

	// The air for what is seen: the sky, the ground light and the clouds. It uses the main
	// gas's own weight and specific heat; the air above (GetAtmPressure etc., flight and
	// everything else) still uses Earth air's for every rocky body. For Earth air the two
	// are the same to the last bit. Joining them is the possible last stage of
	// docs/implementation_plans/atmosphere-types_plan.md.
	// A gas giant's radius is its visible cloud tops, where Jupiter's air is about 0.6 atm (its
	// ammonia clouds): the air model starts there, whatever density the system data gives for
	// the radius (often of 7 atm or more), and goes on down (Planet::GetAtmosphericState).
	// Galileo's probe found some 22 atm 150 km below them; this model gives about that.
	static constexpr double GAS_GIANT_CLOUD_TOP_ATM = 0.6;

	const AtmosphereProfile::Profile &GetAtmosProfile() const { return m_atmosProfile; }
	double GetVisualMolarMass() const { return m_visualAir.molarMass; }
	double GetVisualSpecificHeat() const { return m_visualAir.specificHeat; }
	double GetVisualAtmSurfacePressure() const { return m_visualAir.pressure; }
	double GetVisualAtmRadius() const { return m_visualAir.radius; }
	double GetVisualTropopause() const { return m_visualAir.tropopause; }
	double GetVisualAtmPressure(double altitude) const;
	double GetVisualAtmDensity(double altitude, double pressure) const;

	// the star whose light this body's sky is worked out for: the one it orbits (through its
	// planet, for a moon), or the brightest of a pair it orbits
	const SystemBody *FindLightingStar() const;

	// What this body's air does to light, as the sky (CalcAtmosphereParams), the clouds
	// (PlanetClouds::FillCloudData) and the atmosphere colour (StarSystemGenerator's
	// PickAtmosphere) all need it.
	struct SkyLook {
		float rayleighScale[3] = { 1.0f, 1.0f, 1.0f }; // the gas's scattering as multiples of Earth air's, in this star's light
		float thickAir = 0.0f;						   // 0..1, how far beyond Earth's the air's column is (CalcAtmosphereParams)
		bool earthValues = true;					   // everything as Earth's: the old sums stay, to the last bit
		// optical depth of the air straight up for each atm of pressure above, in the clouds'
		// Earth numbers (0.03, 0.06, 0.13; cloud-march.glsl): its scattering and its gas's absorption
		float depthPerAtm[3] = { 0.03f, 0.06f, 0.13f };
		// the light of the sky on clouds (linear, as Earth's mix of its colour and blue in
		// cloud-march.glsl), and the sky's own colour (brightest channel 1)
		float cloudSkyLight[3] = { 0.0f, 0.0f, 0.0f };
		float skyColour[3] = { 0.0f, 0.0f, 0.0f };
	};
	SkyLook CalcSkyLook() const;

	// Calculate atmosphere pressure at given altitude (atm)
	double GetAtmPressure(double altitude) const;

	// Calculate atmosphere average temperature at given altitude (deg)
	double GetAtmAverageTemp(double altitude) const;

	// Calculate atmosphere density at given altitude and pressure (kg/m^3)
	double GetAtmDensity(double altitude, double pressure) const;

	// for rayleigh scattering
	double ComputeDensity(const double radius, const double atmosphereHeight, const double h, const double scaleHeight) const;
	vector3f GetCoefficients(const double radius, const double atmHeight, const double scaleHeight) const;

	void CalcAtmosphereParams(AtmosphereParameters &params) const;

	bool IsScoopable() const;
	Color CalcSurfaceDustColor() const;
	// how much dust the ground gives up to wheels and jets (0..1): the dry
	// dust and sand of airless and desert worlds; little from wet or living
	// ground, or ice; none off rocky planets and moons
	double CalcSurfaceDustiness() const;

	void Dump(FILE *file, const char *indent = "") const;

	StarSystem *GetStarSystem() const { return m_system; }

	const std::string &GetSpaceStationType() const { return m_spaceStationType; }

private:
	friend class StarSystem;
	friend class ObjectViewerView;
	friend class StarSystemLegacyGeneratorBase;
	friend class StarSystemCustomGenerator;
	friend class StarSystemRandomGenerator;
	friend class PopulateStarSystemGenerator;
	friend class CustomSystemsDatabase;

	// Copy-assignment operator from another instance of SystemBodyData
	SystemBody &operator=(const SystemBodyData &rhs)
	{
		*static_cast<SystemBodyData *>(this) = rhs;
		return *this;
	}

	void SetOrbitFromParameters();
	void SetAtmFromParameters();

	void Orphan();

	SystemBody *m_parent;				  // these are only valid if the StarSystem
	std::vector<SystemBody *> m_children; // that create them still exists
	StarSystem *m_system;

	SystemPath m_path;
	Orbit m_orbit;
	fixed m_orbMin, m_orbMax; // periapsism, apoapsis in AUs
	bool m_isCustomBody;

	// atmosphere surface pressure, unit: atm
	double m_atmosPressure;
	// atmosphere radius at 1e-15atm, unit: meters
	double m_atmosRadius;
	// tropopause height at 0.1atm, unit: meters
	double m_tropopause;

	// the same three for the air that is seen (GetVisualAtmPressure etc.), with its gas
	struct AirColumn {
		double molarMass = 0.0;	   // kg/mol
		double specificHeat = 0.0; // J/(kg K)
		double pressure = 0.0;	   // atm
		double radius = 0.0;	   // m
		double tropopause = 0.0;   // m
	};
	AirColumn m_visualAir;
	AtmosphereProfile::Profile m_atmosProfile;

	void PickAtmosphereProfile(); // part of SetAtmFromParameters
	// the surface density the air model starts from, for a gas of this molar mass (a gas
	// giant's is taken down to its visible cloud tops: GAS_GIANT_CLOUD_TOP_ATM)
	double AirModelDensity(double molarMass) const;
};

#endif // SYSTEMBODY_H
