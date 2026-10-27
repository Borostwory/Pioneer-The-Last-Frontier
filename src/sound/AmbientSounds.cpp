// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "AmbientSounds.h"

#include "CityOnPlanet.h"
#include "Frame.h"
#include "Game.h"
#include "GameConfig.h"
#include "Pi.h"
#include "Planet.h"
#include "surface/Excursion.h"
#include "Player.h"
#include "Random.h"
#include "Sound.h"
#include "Space.h"
#include "SpaceStation.h"
#include "WorldView.h"

enum EAtmosphereNoiseChannels {
	eAtmoNoise1 = 0,
	eAtmoNoise2,
	eAtmoNoise3,
	eMaxNumAtmosphereSounds
};

// airflow04 (high hiss) deliberately unused: too shrill
static const char *s_airflowTable[eMaxNumAtmosphereSounds] = {
	"airflow01",
	"airflow02",
	"airflow03"
};

// Airflow model (mockup). Everything is driven by an "intensity" in 0..1 derived
// from dynamic pressure q = 0.5 * rho * v^2 on a log scale, so it is audible at
// normal flying speeds and still has headroom for hypersonic flight.
//	   q [Pa]   Earth sea level   intensity
//	       30        ~7 m/s          0.0
//	     1500        50 m/s          0.38
//	     6000       100 m/s          0.51
//	    55000       300 m/s          0.72
//	  1000000      1280 m/s          1.0
static const float AIRFLOW_LOG_Q_MIN = 1.48f; // log10(30 Pa)
static const float AIRFLOW_LOG_Q_MAX = 6.0f;  // log10(1 MPa)

// Each layer fades in over [start, end] of intensity; layers are cumulative.
// The bass rumble (airflow01) carries the sound; the mids only add some edge
// at high intensity. Sample RMS for reference: 0.44, 0.07, 0.16.
//	start, end, gain
static const float s_airflowLayerTable[eMaxNumAtmosphereSounds][3] = {
	{ 0.00f, 0.35f, 0.70f }, // airflow01: low rumble (lead)
	{ 0.45f, 0.85f, 0.80f }, // airflow02: mid
	{ 0.60f, 1.00f, 0.50f }	 // airflow03: mid
};

// pitch from airspeed: 0.75x at 10 m/s .. 1.25x at 2000 m/s (log scale), kept low to stay bassy
static const float AIRFLOW_PITCH_LOW = 0.75f;
static const float AIRFLOW_PITCH_HIGH = 1.25f;
static const float AIRFLOW_LOG_V_MIN = 1.0f; // log10(10 m/s)
static const float AIRFLOW_LOG_V_MAX = 3.3f; // log10(2000 m/s)
// pitch from pressure: thin air hisses higher, thick air rumbles lower
// (1 atm: 1.0x, 0.01 atm: 1.2x, 90 atm: 0.8x)
static const float AIRFLOW_PITCH_PER_DECADE_ATM = -0.1f;
// buffeting: volume wobble depth at full intensity, and wobble rate range [Hz]
static const float AIRFLOW_TURBULENCE_DEPTH = 0.15f;
static const float AIRFLOW_TURBULENCE_HZ_LOW = 0.3f;
static const float AIRFLOW_TURBULENCE_HZ_HIGH = 1.5f;
// smoothing time constant for volume and pitch changes [s]
static const float AIRFLOW_SMOOTHING = 0.25f;

// Wind howl: the surface wind heard while hovering, landed, docked or flying
// slowly near the ground of a planet with an atmosphere. Each planet gets a
// sound from its surface conditions (first match wins):
//	dense (> 5 atm), hot (> 400 K) or volcanic   Roar + Sub bed
//	thin (< 0.2 atm)                             Whistle
//	cold (< 240 K) or icy                        MoanA or MoanB (by seed)
//	temperate with life or oxygen                Gusty
//	anything else                                Whistle
// It fades with speed (the airflow layers take over), height above the
// terrain and thinner air, and is silent inside a city's forcefield dome.
struct WindHowlSound {
	const char *sample;
	float gain;
	const char *sub; // sub-bass bed under it, or nullptr
	float subGain;
};
static const WindHowlSound s_windHowlDense = { "WindHowl_Roar", 0.35f, "WindHowl_Sub", 0.4f };
static const WindHowlSound s_windHowlThin = { "WindHowl_Whistle", 0.35f, nullptr, 0.0f };
static const WindHowlSound s_windHowlCold[2] = {
	{ "WindHowl_MoanA", 0.35f, nullptr, 0.0f },
	{ "WindHowl_MoanB", 0.35f, nullptr, 0.0f }
};
static const WindHowlSound s_windHowlTemperate = { "WindHowl_Gusty", 0.35f, nullptr, 0.0f };

static const double WINDHOWL_DENSE_ATM = 5.0;
static const int WINDHOWL_HOT_K = 400;
static const double WINDHOWL_VOLCANIC = 0.5;
static const double WINDHOWL_THIN_ATM = 0.2;
static const int WINDHOWL_COLD_K = 240;
static const double WINDHOWL_ICY = 0.3;
static const int WINDHOWL_TEMPERATE_MAX_K = 330;
static const double WINDHOWL_LIFE = 0.1;
static const double WINDHOWL_OXIDIZING = 0.5;

// level from air density at the ship, log scale: 0.3 at 0.001 kg/m^3 (silent
// below) .. 1.0 at 10 kg/m^3 (Mars ~0.5, Earth ~0.85)
static const double WINDHOWL_MIN_DENSITY = 0.001;
static const float WINDHOWL_LOG_DENSITY_MIN = -3.0f;
static const float WINDHOWL_LOG_DENSITY_MAX = 1.0f;
static const float WINDHOWL_DENSITY_LEVEL_MIN = 0.3f;
// full up to the first speed [m/s] / height above terrain [m], gone at the second
static const float WINDHOWL_SPEED_FULL = 15.0f;
static const float WINDHOWL_SPEED_GONE = 120.0f;
static const float WINDHOWL_ALTITUDE_FULL = 1500.0f;
static const float WINDHOWL_ALTITUDE_GONE = 6000.0f;
// gusts: a new volume target in [min, 1] every few seconds; slow pitch drift
// on the main sound so the loop doesn't repeat exactly
static const float WINDHOWL_GUST_MIN = 0.6f;
static const float WINDHOWL_GUST_INTERVAL_MIN = 2.0f; // s
static const float WINDHOWL_GUST_INTERVAL_MAX = 6.0f;
static const float WINDHOWL_GUST_SMOOTHING = 1.2f; // s
static const float WINDHOWL_PITCH_DRIFT = 0.05f;   // +-
static const float WINDHOWL_PITCH_INTERVAL_MIN = 3.0f;
static const float WINDHOWL_PITCH_INTERVAL_MAX = 8.0f;
static const float WINDHOWL_PITCH_SMOOTHING = 2.5f;
// fade in/out time constant, also used when switching sounds [s]
static const float WINDHOWL_SMOOTHING = 1.5f;
// stations looked at for a forcefield the player may be inside [m]
static const double WINDHOWL_FORCEFIELD_SEARCH = 20000.0;

static const Uint32 NUM_SURFACE_LIFE_SOUNDS = 12;
static const char *s_surfaceLifeSounds[NUM_SURFACE_LIFE_SOUNDS] = {
	"Wind", "Thunder_1", "Thunder_2", "Thunder_3",
	"Thunder_4", "Storm", "Rain_Light", "River",
	"RainForestIntroducedNight", "RainForestIntroduced",
	"NormalForestIntroduced", "RainForestIndigeniusNight"
};

static const Uint32 NUM_SURFACE_DEAD_SOUNDS = 12;
static const char *s_surfaceSounds[NUM_SURFACE_DEAD_SOUNDS] = {
	"Wind", "Thunder_1", "Thunder_2", "Thunder_3",
	"Thunder_4", "Storm"
};

static const Uint32 NUM_STATION_SOUNDS = 3;
static const char *s_stationNoiseSounds[NUM_STATION_SOUNDS] = {
	"Large_Station_ambient",
	"Medium_Station_ambient",
	"Small_Station_ambient"
};

static int astroNoiseSeed;
static Sound::Event s_stationNoise;
static Sound::Event s_starNoise;
static Sound::Event s_atmosphereNoises[eMaxNumAtmosphereSounds];
static float s_airflowVolumes[eMaxNumAtmosphereSounds];
static float s_airflowPitch = 1.0f;
static float s_airflowTurbulencePhase = 0.0f;
static Sound::Event s_planetSurfaceNoise;
static Sound::Event s_windHowl;
static Sound::Event s_windHowlSub;
static const char *s_windHowlPlaying = nullptr;
static const char *s_windHowlSubPlaying = nullptr;
static float s_windHowlVolume = 0.0f;
static float s_windHowlSubVolume = 0.0f;
static float s_windGust = 1.0f;
static float s_windGustTarget = 1.0f;
static float s_windGustTimer = 0.0f;
static float s_windPitch = 1.0f;
static float s_windPitchTarget = 1.0f;
static float s_windPitchTimer = 0.0f;
static Random s_windRng;
static sigc::connection onChangeCamTypeConnection;

static const WindHowlSound &ChooseWindHowl(const SystemBody *sbody)
{
	const double pressure = sbody->GetAtmSurfacePressure();
	const int temp = sbody->GetAverageTemp();
	if (pressure > WINDHOWL_DENSE_ATM || temp > WINDHOWL_HOT_K || sbody->GetVolcanicity() > WINDHOWL_VOLCANIC)
		return s_windHowlDense;
	if (pressure < WINDHOWL_THIN_ATM)
		return s_windHowlThin;
	if (temp < WINDHOWL_COLD_K || sbody->GetVolatileIces() > WINDHOWL_ICY)
		return s_windHowlCold[sbody->GetSeed() % 2];
	if (temp <= WINDHOWL_TEMPERATE_MAX_K && (sbody->GetLife() > WINDHOWL_LIFE || sbody->GetAtmosOxidizing() > WINDHOWL_OXIDIZING))
		return s_windHowlTemperate;
	return s_windHowlThin;
}

static bool IsPlayerInCityForcefield()
{
	for (Body *body : Pi::game->GetSpace()->GetBodiesMaybeNear(Pi::player, WINDHOWL_FORCEFIELD_SEARCH)) {
		if (!body->IsType(ObjectType::SPACESTATION))
			continue;
		const CityOnPlanet *city = static_cast<const SpaceStation *>(body)->GetAdjacentCity();
		if (city && city->IsPlayerInsideForcefield())
			return true;
	}
	return false;
}

// Fades one wind howl event towards target, starting it if needed. When a
// different sample is wanted, the current one fades out and is stopped first.
static void UpdateWindHowlEvent(Sound::Event &ev, const char *&playing, float &volume, const char *wanted, float target, float pitch, float smoothing)
{
	if (!ev.IsPlaying()) {
		playing = nullptr;
		volume = 0.0f;
	}
	if (playing && playing != wanted)
		target = 0.0f;
	if (!playing) {
		if (!wanted || target <= 0.0f)
			return;
		// SetVolume (unlike Play) applies the sfx volume once, matching v_env
		ev.Play(wanted, 0.0f, 0.0f, Sound::OP_REPEAT);
		playing = wanted;
	}
	volume += (target - volume) * smoothing;
	if (target <= 0.0f && volume < 0.002f) {
		ev.Stop();
		playing = nullptr;
		volume = 0.0f;
		return;
	}
	ev.SetVolume(volume);
	ev.SetPitch(pitch);
}

static void UpdateWindHowl(const float v_env)
{
	const float dt = Pi::GetFrameTime();
	const WindHowlSound *sound = nullptr;
	float level = 0.0f;

	// where the player is: the ship, or the walker out of it
	Body *listener = Pi::game->GetControlledBody();
	Frame *playerFrame = Pi::game->IsNormalSpace() ? Frame::GetFrame(listener->GetFrame()) : nullptr;
	const Body *astro = playerFrame ? playerFrame->GetBody() : nullptr;
	if (astro && playerFrame->IsRotFrame() && astro->IsType(ObjectType::PLANET)) {
		const Planet *planet = static_cast<const Planet *>(astro);
		double pressure, density;
		planet->GetAtmosphericState(listener->GetPosition().Length(), &pressure, &density);
		if (density > WINDHOWL_MIN_DENSITY) {
			sound = &ChooseWindHowl(planet->GetSystemBody());
			const float densityLevel = Clamp((float(log10(density)) - WINDHOWL_LOG_DENSITY_MIN) / (WINDHOWL_LOG_DENSITY_MAX - WINDHOWL_LOG_DENSITY_MIN), 0.0f, 1.0f);
			// velocity in the planet's rotating frame is airspeed (the atmosphere co-rotates)
			const float speed = float(listener->GetVelocity().Length());
			const float altitude = float(listener->GetAltitudeRelTo(planet, AltitudeType::ABOVE_TERRAIN));
			level = (WINDHOWL_DENSITY_LEVEL_MIN + (1.0f - WINDHOWL_DENSITY_LEVEL_MIN) * densityLevel) *
				(1.0f - Clamp((speed - WINDHOWL_SPEED_FULL) / (WINDHOWL_SPEED_GONE - WINDHOWL_SPEED_FULL), 0.0f, 1.0f)) *
				(1.0f - Clamp((altitude - WINDHOWL_ALTITUDE_FULL) / (WINDHOWL_ALTITUDE_GONE - WINDHOWL_ALTITUDE_FULL), 0.0f, 1.0f));
			if (level > 0.0f && IsPlayerInCityForcefield())
				level = 0.0f;
		}
	}

	s_windGustTimer -= dt;
	if (s_windGustTimer <= 0.0f) {
		s_windGustTimer = float(s_windRng.Double(WINDHOWL_GUST_INTERVAL_MIN, WINDHOWL_GUST_INTERVAL_MAX));
		s_windGustTarget = float(s_windRng.Double(WINDHOWL_GUST_MIN, 1.0));
	}
	s_windGust += (s_windGustTarget - s_windGust) * (1.0f - expf(-dt / WINDHOWL_GUST_SMOOTHING));

	s_windPitchTimer -= dt;
	if (s_windPitchTimer <= 0.0f) {
		s_windPitchTimer = float(s_windRng.Double(WINDHOWL_PITCH_INTERVAL_MIN, WINDHOWL_PITCH_INTERVAL_MAX));
		s_windPitchTarget = float(s_windRng.Double(1.0 - WINDHOWL_PITCH_DRIFT, 1.0 + WINDHOWL_PITCH_DRIFT));
	}
	s_windPitch += (s_windPitchTarget - s_windPitch) * (1.0f - expf(-dt / WINDHOWL_PITCH_SMOOTHING));

	// WindHowlVolume setting, percent (settings window, sound)
	level *= Clamp(Pi::config->Int("WindHowlVolume", 50), 0, 200) * 0.01f;

	const float smoothing = 1.0f - expf(-dt / WINDHOWL_SMOOTHING);
	UpdateWindHowlEvent(s_windHowl, s_windHowlPlaying, s_windHowlVolume,
		sound ? sound->sample : nullptr,
		sound ? sound->gain * level * s_windGust * v_env : 0.0f,
		s_windPitch, smoothing);
	// the sub bed follows the gusts only half as much and keeps its pitch
	UpdateWindHowlEvent(s_windHowlSub, s_windHowlSubPlaying, s_windHowlSubVolume,
		sound ? sound->sub : nullptr,
		sound ? sound->subGain * level * (0.5f + 0.5f * s_windGust) * v_env : 0.0f,
		1.0f, smoothing);
}

void AmbientSounds::Init()
{
	onChangeCamTypeConnection = Pi::game->GetWorldView()->shipView->onChangeCamType.connect(sigc::ptr_fun(&AmbientSounds::UpdateForCamType));
}

void AmbientSounds::Uninit()
{
	onChangeCamTypeConnection.disconnect();
}

void AmbientSounds::Update()
{
	// muffled inside the ship, full outside it
	const bool exterior = Pi::game->GetWorldView()->shipView->IsExteriorView() || Pi::game->GetExcursion()->IsOffShip();
	const float v_env = (exterior ? 1.0f : 0.5f) * Sound::GetSfxVolume();

	UpdateWindHowl(v_env);

	if (Pi::player->GetFlightState() == Ship::DOCKED) {
		if (s_starNoise.IsPlaying()) {
			s_starNoise.FadeOut(1.0f, Sound::OP_REPEAT);
		}
		for (int i = 0; i < eMaxNumAtmosphereSounds; i++) {
			if (s_atmosphereNoises[i].IsPlaying()) {
				s_atmosphereNoises[i].FadeOut(1.0f, Sound::OP_REPEAT);
			}
		}
		if (s_planetSurfaceNoise.IsPlaying()) {
			s_planetSurfaceNoise.FadeOut(1.0f, Sound::OP_REPEAT);
		}

		if (!s_stationNoise.IsPlaying()) {
			// just use a random station noise until we have a
			// concept of 'station size'
			s_stationNoise.Play(s_stationNoiseSounds[Pi::player->GetDockedWith()->GetSystemBody()->GetSeed() % NUM_STATION_SOUNDS],
				0.3f * v_env, 0.3f * v_env, Sound::OP_REPEAT);
		}
	} else if (Pi::player->GetFlightState() == Ship::LANDED) {
		/* Planet surface noise on rough-landing */
		if (s_starNoise.IsPlaying()) {
			s_starNoise.FadeOut(1.0f, Sound::OP_REPEAT);
		}
		for (int i = 0; i < eMaxNumAtmosphereSounds; i++) {
			if (s_atmosphereNoises[i].IsPlaying()) {
				s_atmosphereNoises[i].FadeOut(1.0f, Sound::OP_REPEAT);
			}
		}
		if (s_stationNoise.IsPlaying()) {
			s_stationNoise.FadeOut(1.0f, Sound::OP_REPEAT);
		}

		// lets try something random for the time being
		if (!s_planetSurfaceNoise.IsPlaying()) {
			const SystemBody *sbody = Frame::GetFrame(Pi::player->GetFrame())->GetSystemBody();
			assert(sbody);
			const char *sample = 0;

			if (sbody->GetLifeAsFixed() > fixed(1, 5)) {
				sample = s_surfaceLifeSounds[sbody->GetSeed() % NUM_SURFACE_LIFE_SOUNDS];
			} else if (sbody->GetVolatileGasAsFixed() > fixed(1, 2)) {
				sample = s_surfaceSounds[sbody->GetSeed() % NUM_SURFACE_DEAD_SOUNDS];
			} else if (sbody->GetVolatileGasAsFixed() > fixed(1, 10)) {
				sample = "Wind";
			}

			if (sample) {
				s_planetSurfaceNoise.Play(sample, 0.3f * v_env, 0.3f * v_env, Sound::OP_REPEAT);
			}
		}
	} else if (s_planetSurfaceNoise.IsPlaying()) {
		// s_planetSurfaceNoise.IsPlaying() - if player is no longer on the ground then stop playing
		Frame *playerFrame = Frame::GetFrame(Pi::player->GetFrame());
		if (playerFrame->IsRotFrame()) {
			const Body *astro = playerFrame->GetBody();
			if (astro->IsType(ObjectType::PLANET)) {
				const double dist = Pi::player->GetPosition().Length();
				double pressure, density;
				static_cast<const Planet *>(astro)->GetAtmosphericState(dist, &pressure, &density);
				if (Pi::player->GetFlightState() != Ship::LANDED) {
					// Stop playing surface noise once the ship is off the ground
					s_planetSurfaceNoise.Stop();
				}
			}
		} else if (Pi::game->IsHyperspace()) {
			s_planetSurfaceNoise.Stop();
		}
	} else {
		if (s_stationNoise.IsPlaying()) {
			s_stationNoise.FadeOut(1.0f, Sound::OP_REPEAT);
		}
		if (Pi::game->IsNormalSpace()) {
			StarSystem *s = Pi::game->GetSpace()->GetStarSystem().Get();
			if (astroNoiseSeed != s->GetSeed()) {
				// change sound!
				astroNoiseSeed = s->GetSeed();
				s_starNoise.FadeOut(0.1f, Sound::OP_REPEAT);
				// XXX the way Sound::Event works isn't totally obvious.
				// to destroy the object doesn't stop the sound. it is
				// really just a sound event reference
				s_starNoise = Sound::Event();
			}
		}
		// when all the sounds are in we can use the body we are in frame of reference to
		if (!s_starNoise.IsPlaying()) {
			Frame *f = Frame::GetFrame(Pi::player->GetFrame());
			if (!f) return; // When player has no frame (game abort) then get outta here!!
			const SystemBody *sbody = f->GetSystemBody();
			const char *sample = 0;
			for (; sbody && !sample; sbody = f->GetSystemBody()) {
				switch (sbody->GetType()) {
				case SystemBody::TYPE_BROWN_DWARF: sample = "Brown_Dwarf_Substellar_Object"; break;
				case SystemBody::TYPE_STAR_M: sample = "M_Red_Star"; break;
				case SystemBody::TYPE_STAR_K: sample = "K_Star"; break;
				case SystemBody::TYPE_WHITE_DWARF: sample = "White_Dwarf_Star"; break;
				case SystemBody::TYPE_STAR_G: sample = "G_Star"; break;
				case SystemBody::TYPE_STAR_F: sample = "F_Star"; break;
				case SystemBody::TYPE_STAR_A: sample = "A_Star"; break;
				case SystemBody::TYPE_STAR_B: sample = "B_Hot_Blue_STAR"; break;
				case SystemBody::TYPE_STAR_O: sample = "Blue_Super_Giant"; break;
				case SystemBody::TYPE_PLANET_GAS_GIANT: {
					if (sbody->GetMassAsFixed() > fixed(400, 1)) {
						sample = "Very_Large_Gas_Giant";
					} else if (sbody->GetMassAsFixed() > fixed(80, 1)) {
						sample = "Large_Gas_Giant";
					} else if (sbody->GetMassAsFixed() > fixed(20, 1)) {
						sample = "Medium_Gas_Giant";
					} else {
						sample = "Small_Gas_Giant";
					}
				} break;
				default: sample = 0; break;
				}
				if (sample) {
					s_starNoise.Play(sample, 0.0f, 0.0f, Sound::OP_REPEAT);
					s_starNoise.VolumeAnimate(.3f * v_env, .3f * v_env, .05f, .05f);
				} else {
					// go up orbital hierarchy tree to see if we can find a sound
					FrameId parent = f->GetParent();
					f = Frame::GetFrame(parent);
					if (f == nullptr) break;
				}
			}
		}

		Frame *playerFrame = Frame::GetFrame(Pi::player->GetFrame());
		const Body *astro = playerFrame->GetBody();
		if (astro && playerFrame->IsRotFrame() && (astro->IsType(ObjectType::PLANET))) {
			double dist = Pi::player->GetPosition().Length();
			double pressure, density;
			static_cast<const Planet *>(astro)->GetAtmosphericState(dist, &pressure, &density);
			// velocity in the planet's rotating frame is airspeed (the atmosphere co-rotates)
			const double airspeed = Pi::player->GetVelocity().Length();
			const double dynamicPressure = 0.5 * density * airspeed * airspeed;

			const float intensity = dynamicPressure > 0.0 ?
				Clamp((float(log10(dynamicPressure)) - AIRFLOW_LOG_Q_MIN) / (AIRFLOW_LOG_Q_MAX - AIRFLOW_LOG_Q_MIN), 0.0f, 1.0f) :
				0.0f;
			const float speedNorm = airspeed > 0.0 ?
				Clamp((float(log10(airspeed)) - AIRFLOW_LOG_V_MIN) / (AIRFLOW_LOG_V_MAX - AIRFLOW_LOG_V_MIN), 0.0f, 1.0f) :
				0.0f;

			float targetPitch = AIRFLOW_PITCH_LOW + (AIRFLOW_PITCH_HIGH - AIRFLOW_PITCH_LOW) * speedNorm;
			if (pressure > 0.0)
				targetPitch *= Clamp(1.0f + AIRFLOW_PITCH_PER_DECADE_ATM * float(log10(pressure)), 0.8f, 1.2f);

			// buffeting: two incommensurate sines, faster and deeper with speed and pressure
			const float dt = Pi::GetFrameTime();
			const float turbulenceHz = AIRFLOW_TURBULENCE_HZ_LOW + (AIRFLOW_TURBULENCE_HZ_HIGH - AIRFLOW_TURBULENCE_HZ_LOW) * speedNorm;
			s_airflowTurbulencePhase = fmodf(s_airflowTurbulencePhase + float(2.0 * M_PI) * turbulenceHz * dt, float(2.0 * M_PI) * 100.0f);
			const float turbulence = 1.0f + AIRFLOW_TURBULENCE_DEPTH * intensity * (0.6f * sinf(s_airflowTurbulencePhase) + 0.4f * sinf(2.37f * s_airflowTurbulencePhase + 1.3f));

			const float smoothing = 1.0f - expf(-dt / AIRFLOW_SMOOTHING);
			s_airflowPitch += (targetPitch - s_airflowPitch) * smoothing;

			for (int i = 0; i < eMaxNumAtmosphereSounds; i++) {
				const float beg = s_airflowLayerTable[i][0];
				const float end = s_airflowLayerTable[i][1];
				const float gain = s_airflowLayerTable[i][2];
				const float target = Clamp((intensity - beg) / (end - beg), 0.0f, 1.0f) * gain * turbulence * v_env;

				if (!s_atmosphereNoises[i].IsPlaying()) {
					// start silent and let the smoothing bring it up; SetVolume (unlike Play)
					// applies the sfx volume once, matching v_env
					s_atmosphereNoises[i].Play(s_airflowTable[i], 0.0f, 0.0f, Sound::OP_REPEAT);
					s_airflowVolumes[i] = 0.0f;
				}
				s_airflowVolumes[i] += (target - s_airflowVolumes[i]) * smoothing;
				s_atmosphereNoises[i].SetVolume(s_airflowVolumes[i]);
				// spread the layers slightly so identical pitches don't phase
				s_atmosphereNoises[i].SetPitch(s_airflowPitch * (0.98f + 0.02f * i));
			}
		} else {
			for (int i = 0; i < eMaxNumAtmosphereSounds; i++) {
				s_atmosphereNoises[i].FadeOut(1.0f, Sound::OP_REPEAT);
			}
		}
	}
}

void AmbientSounds::UpdateForCamType()
{
	const ShipViewController::CamType cam = Pi::game->GetWorldView()->shipView->GetCamType();
	float v_env = (cam == ShipViewController::CAM_EXTERNAL ? 1.0f : 0.5f) * Sound::GetSfxVolume();

	if (s_stationNoise.IsPlaying())
		s_stationNoise.SetVolume(0.3f * v_env, 0.3f * v_env);
	if (s_starNoise.IsPlaying())
		s_starNoise.SetVolume(0.3f * v_env, 0.3f * v_env);
	if (s_planetSurfaceNoise.IsPlaying())
		s_planetSurfaceNoise.SetVolume(0.3f * v_env, 0.3f * v_env);
}
