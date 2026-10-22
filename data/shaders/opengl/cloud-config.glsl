// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

// Volumetric cloud tuning constants.
// Edit, copy the shaders into the game's data folder and restart the game.
// NOTE: the cloud-*.glsl files must not pull in other shader files themselves,
// the engine rejects any file that is included twice.

// Debug views (0 = off)
//   1 latitude/longitude grid (equator red, 0 deg longitude green, 30 N blue)
//   2 layer coverage (R = Low, G = Medium, B = High)
//   3 species (H2SO4 yellow, H2O blue, CH4 orange, NH3 green, CO2 white, none black)
//   4 vertical optical depth heat map (black 0 -> blue -> green -> red 40)
//   5 low layer genus (stratus white, stratocumulus red)
//   6 wind at the medium layer (cyan prograde, magenta retrograde, brightness = speed / 100 m/s)
//   7 terrain floor (red where clouds are pushed up by terrain + 750 m)
#define CLOUD_DEBUG 0

// Maximum raymarch steps per layer segment (the cloud pass runs at reduced
// resolution, see CloudResolutionDivisor in config.ini)
#define CLOUD_STEPS 48

// Cloud cell sizes in metres for an Earth-like atmosphere (scaled by the
// planet's scale height): low (stratocumulus), medium (altocumulus), high (cirrus)
#define CLOUD_CELL_LOW_M 4000.0
#define CLOUD_CELL_MID_M 1500.0
#define CLOUD_CELL_HIGH_M 5000.0

// Cells smaller than this many pixels on screen fade to the mean density of
// the field (avoids speckled "freckle" clouds seen from far away)
#define CLOUD_MIN_CELL_PIXELS 4.0

// Noise octaves near the camera (they fade out with distance)
#define CLOUD_SHAPE_OCTAVES 3
#define CLOUD_DETAIL_OCTAVES 2

// Beyond this distance (m) along a layer segment it is rendered as a single slab
#define CLOUD_MAX_MARCH_DIST 40000.0

// Distance (m) at which clouds have faded to 1/e into the sky haze, for a
// camera at 1 atm; it grows in thinner air. Clouds beyond 4x are skipped.
#define CLOUD_VISIBILITY 90000.0

// Stop marching once the view transmittance drops below this
#define CLOUD_MIN_TRANSMITTANCE 0.02

// Cloud shadows on terrain (0/1)
#define CLOUD_TERRAIN_SHADOWS 1

// Brightness variation between individual clouds (fraction)
#define CLOUD_TINT_AMPLITUDE 0.04

// Global coverage multiplier for art tuning
#define CLOUD_COVERAGE_SCALE 1.0

// Hard terrain clearance in metres (do not lower)
#define CLOUD_FLOOR_M 750.0

// Brightness of cloud light before tonemapping
#define CLOUD_EXPOSURE 1.6

// Strength of sky-coloured ambient light on clouds
#define CLOUD_AMBIENT 0.35

// Wind direction convention: set to -1.0 if clouds drift westward at
// Earth's mid-latitudes (they should drift eastward)
#define CLOUD_WIND_SIGN 1.0

// Maximum number of stars that light clouds
#define CLOUD_MAX_LIGHTS 2

// Measured distributions of the shape noises (mean, sd), used to map them to
// uniform variables. Re-measure if the noise functions change.
#define CLOUD_PW_MEAN 0.704
#define CLOUD_PW_SD 0.096
#define CLOUD_BILLOW_MEAN 0.754
#define CLOUD_BILLOW_SD 0.125
