// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "AmbientSounds.h"

#include "Frame.h"
#include "Game.h"
#include "Pi.h"
#include "Planet.h"
#include "Player.h"
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
static sigc::connection onChangeCamTypeConnection;

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
	const float v_env = (Pi::game->GetWorldView()->shipView->IsExteriorView() ? 1.0f : 0.5f) * Sound::GetSfxVolume();

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
