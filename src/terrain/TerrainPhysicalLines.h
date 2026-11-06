// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef _TERRAINPHYSICALLINES_H
#define _TERRAINPHYSICALLINES_H

#include "PlanetPhysics.h"

#include "../vector3.h"

#include <cstdint>
#include <utility>
#include <vector>

// The tectonic lines of the physical surface generator (planet lab: fields.py, catalogue and the lines of stage 1):
// the bands, ridges and fractures of an active ice shell, the lobate scarps of a shrinking body, and old fault
// valleys. They are curves in a list made when the body is built, wandering along a stress field that turns between
// generations, so they cross each other, and a younger line cuts through an older one. How many there are and how
// strong comes from the interior's tides and cooling, never from the ice on the surface.
//
// An active shell like Europa's is drawn by long arcs in three layers, a few strong reddish ones, many thinner, and a
// faint mesh; short lines only branch off long ones. Sizes: fractures 100 m to 2 km wide, 100 m to a few km deep; double ridges 1 to 3 km
// across with a trough of 200 to 500 m, 100 to 300 m tall; bands 10 to 30 km wide and up to some 200 m deep.
//
// Read-only after Build, and safe to read from any thread.
class TerrainLines {
public:
	struct Sample {
		double height = 0.0; // metres
		double crack = 0.0; // 0..1, a thin dark line
		double band = 0.0; // 0..1, the reddish salts of a band or a ridge's flanks
		double pale = 0.0; // 0..1, clean pale ice, on domes
		double dark = 0.0; // 0..1, the dark ice of a band
		double slot = 0.0; // 0..1, the coloured floor of a deep crack, and its walls going down
		bool slotBlue = false; // that colour is blue ice, not rust
	};

	void Build(const PlanetPhysics::Core &core, const PlanetPhysics::Interior &interior, uint32_t noiseSeed);

	// p on the unit sphere. Follows the band limit of the calling thread.
	Sample At(const vector3d &p) const;

	// an active shell, with its salts and deep fractures
	bool IsActiveShell() const { return m_activeShell; }
	double Tallest() const { return m_tallest; }
	double Deepest() const { return m_deepest; }
	size_t LineCount() const { return m_middles.size(); }
	size_t FineCount() const { return m_fineCount; }
	size_t DomeCount() const { return m_domes.size(); }
	// where the salts lie thickest, in a soft stain over one side
	const vector3d &StainAxis() const { return m_stainAxis; }
	// the middle of each large line, and its length on the unit sphere, for the debug finder
	const std::vector<std::pair<vector3d, double>> &Middles() const { return m_middles; }

private:
	enum Kind : uint8_t {
		RIDGE, // a sharp trough between two low crests
		GRABEN, // two facing walls and a dropped floor, wide
		SCARP, // a one-sided step, with a steep front and a long gentle back
		BAND, // a wide flat lowered ribbon with fine lines along it
		FRACTURE // a narrow crack with steep walls
	};
	// Lines finer than the samples are drawn as wide as the samples, and listed that far around, so each size is
	// listed in its own grid and fades out at its own spacing.
	enum Tier {
		LARGE,
		MEDIUM,
		MESH,
		TIER_COUNT
	};
	struct Line {
		Kind kind;
		int8_t generation; // a younger line cuts an older one
		float width; // on the unit sphere
		float height; // metres
		float pieces;
		float tone; // how strongly it is coloured, 0..1
		bool wedge = false; // it widens from one end to the other
		bool blue = false; // its floor is blue ice
	};
	struct Piece {
		vector3d a, b;
		double lengthSqr;
		int line;
		float first; // how many pieces before this one on its line
	};
	struct Dome {
		vector3d centre;
		double radius; // on the unit sphere
		double cosReach;
		double height; // metres
		bool freckle; // a small dark reddish pit, or a low pale dome
	};
	// which items reach into each cell of a latitude-longitude grid
	struct Index {
		int rows = 0, columns = 0;
		std::vector<std::vector<uint32_t>> cells;
		void Resize(int r, int c);
		void Add(const vector3d &a, const vector3d &b, double margin, uint32_t id);
		const std::vector<uint32_t> *At(const vector3d &p) const;
	};
	struct Nearest {
		double distance = 1e30; // in widths
		double carve = 0.0; // of a crack: metres
		double width = 1.0;
		double height = 0.0;
		double fade = 1.0;
		double tone = 1.0;
		double coverage = 1.0; // how much of the width drawn the real line fills
		double side = 0.0;
		double rim = 0.0; // of a fault valley: metres, how high its rim is
		bool blue = false;
		int generation = -100;
		Kind kind = RIDGE;
	};

	void AddLine(const std::vector<vector3d> &points, Kind kind, int generation, double width, double height, double tone, Tier tier);
	void BuildActiveShell(const PlanetPhysics::Core &core, double strength, uint32_t seed);
	// top: what is drawn here; under: an older line it cuts through, which shows beyond its edge; deepest: the crack cut deepest here
	void Gather(const std::vector<uint32_t> &nearby, const vector3d &p, const vector3d &lobed, double spacing, double tierFade, Nearest &top, Nearest &under, Nearest &deepest) const;
	// how far from its middle, in widths, a line covers what is under it
	static double CutReach(Kind kind);
	// how much of its depth a crack has at a distance, 0..1
	static double CrackShape(const Nearest &crack, double spacing);
	// how much of its depth a fault valley has at a distance, 0..1
	static double GrabenCut(const Nearest &line, double spacing);
	void Draw(const Nearest &line, const vector3d &p, double spacing, double scale, Sample &out) const;

	std::vector<Line> m_lines;
	std::vector<Piece> m_pieces;
	std::vector<Dome> m_domes;
	Index m_index[TIER_COUNT], m_domeIndex;
	std::vector<std::pair<vector3d, double>> m_middles;
	size_t m_fineCount = 0;
	vector3d m_stainAxis = vector3d(0.0, 1.0, 0.0);
	double m_tallest = 0.0, m_deepest = 0.0;
	uint32_t m_noiseSeed = 0;
	bool m_lobed = false; // it has lines whose edges wander in close views
	bool m_activeShell = false;
};

#endif // _TERRAINPHYSICALLINES_H
