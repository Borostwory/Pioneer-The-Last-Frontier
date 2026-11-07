// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#ifndef DEATH_VIEW_H
#define DEATH_VIEW_H

#include "RefCounted.h"
#include "View.h"
#include "vector3.h"

class Camera;
class CameraContext;
class Game;
class TerrainBody;

class DeathView : public View {
public:
	DeathView(Game *game);
	virtual ~DeathView();

	void Init();

	virtual void Update();
	virtual void Draw3D();

protected:
	virtual void OnSwitchTo();

private:
	void PlaceCamera();

	RefCountedPtr<CameraContext> m_cameraContext;
	std::unique_ptr<Camera> m_camera;
	float m_cameraDist;
	// the way the camera backs off from the body, in its frame
	vector3d m_cameraDir;
	// the planet it died on, if in its rotating frame: the camera keeps
	// above its ground and upright with it
	const TerrainBody *m_terrain = nullptr;
	Game *m_game;
};

#endif
