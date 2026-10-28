// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#pragma once

#include "graphics/RenderState.h"
#include "graphics/Renderer.h"
#include "graphics/Types.h"
#include "graphics/VertexBuffer.h"
#include "graphics/UniformBuffer.h"
#include "OpenGLLibs.h"

typedef void *SDL_GLContext;

namespace Graphics {

	class Texture;
	struct Settings;

	namespace OGL {
		class CommandList;
		class IndexBuffer;
		class Material;
		class MeshObject;
		class RenderState;
		class RenderStateCache;
		class RenderTarget;
		class Shader;
		class UniformBuffer;
		class UniformLinearBuffer;
		class VertexBuffer;
	} // namespace OGL

	class RendererOGL final : public Renderer {
	public:
		static void RegisterRenderer();

		RendererOGL(SDL_Window *window, const Graphics::Settings &vs, SDL_GLContext &glContext);
		~RendererOGL() final;

		const char *GetName() const final;
		RendererType GetRendererType() const final { return RENDERER_OPENGL_3x; }

		void WriteRendererInfo(std::ostream &out) const final;

		void CheckRenderErrors(const char *func = nullptr, const int line = -1) const final { CheckErrors(func, line); }
		static void CheckErrors(const char *func = nullptr, const int line = -1);

		bool SupportsInstancing() final { return true; }

		int GetMaximumNumberAASamples() const final;
		bool GetNearFarRange(float &near_, float &far_) const final;

		void SetVSyncEnabled(bool) override;

		void OnWindowResized() override;

		bool BeginFrame() final;
		bool EndFrame() final;
		bool SwapBuffers() final;

		RenderTarget *GetRenderTarget() final;
		bool SetRenderTarget(RenderTarget *) final;
		bool SetScissor(ViewportExtents) final;

		void CopyRenderTarget(RenderTarget *, RenderTarget *, ViewportExtents, ViewportExtents, bool) final;
		void ResolveRenderTarget(RenderTarget *, RenderTarget *, ViewportExtents) final;

		bool ClearScreen(const Color &c, bool) final;
		bool ClearDepthBuffer() final;

		bool SetViewport(ViewportExtents v) final;
		ViewportExtents GetViewport() const final { return m_viewport; }

		bool SetTransform(const matrix4x4f &m) final;
		matrix4x4f GetTransform() const final { return m_modelViewMat; }

		bool SetPerspectiveProjection(float fov, float aspect, float near_, float far_) final;
		bool SetOrthographicProjection(float xmin, float xmax, float ymin, float ymax, float zmin, float zmax) final;
		bool SetProjection(const matrix4x4f &m) final;
		matrix4x4f GetProjection() const final { return m_projectionMat; }

		bool SetWireFrameMode(bool enabled) final;

		bool SetLightIntensity(Uint32 numlights, const float *intensity) final;
		bool SetLights(Uint32 numlights, const Light *l) final;
		Uint32 GetNumLights() const final { return m_numLights; }
		bool SetAmbientColor(const Color &c) final;
		void SetShadowData(const ShadowDataBlock *data, Texture *const *maps) final;
		void SetCityLightData(const CityLightDataBlock *data, Texture *lightMap, Texture *lightGrid) final;
		void SetCityGroundData(const CityGroundDataBlock *data, Texture *roads, Texture *lots) final;
		void SetSurfaceTrackData(const SurfaceTrackDataBlock *data, Texture *tracks) final;
		void SetShipLightData(const ShipLightDataBlock *data) final;

		bool FlushCommandBuffers() final;

		void BeginGpuTimer(const char *name) final;
		void EndGpuTimer() final;
		const std::vector<GpuTiming> &GetGpuTimings() const final { return m_gpuTimings; }

		bool DrawBuffer(const VertexArray *v, Material *m) final;
		bool DrawMesh(MeshObject *, Material *) final;

		void Draw(Span<const BufferBinding<VertexBuffer>>, BufferBinding<IndexBuffer>, Material *m, uint32_t, uint32_t) final;

		Material *CreateMaterial(const std::string &, const MaterialDescriptor &, const RenderStateDesc &, const VertexFormatDesc &) final;
		Material *CloneMaterial(const Material *, const MaterialDescriptor &, const RenderStateDesc &, const VertexFormatDesc &) final;
		Texture *CreateTexture(const TextureDescriptor &descriptor) final;
		RenderTarget *CreateRenderTarget(const RenderTargetDesc &) final;
		VertexBuffer *CreateVertexBuffer(BufferUsage, uint32_t, uint32_t) final;
		IndexBuffer *CreateIndexBuffer(Uint32 size, BufferUsage, IndexBufferSize) final;
		UniformBuffer *CreateUniformBuffer(Uint32 size, BufferUsage) final;
		MeshObject *CreateMeshObject(const VertexFormatDesc &desc, VertexBuffer *v, IndexBuffer *i) final;
		MeshObject *CreateMeshObjectFromArray(const VertexArray *v, IndexBuffer *i = nullptr, BufferUsage u = BUFFER_USAGE_STATIC) final;

		BufferBinding<VertexBuffer> CreateTempVertexBuffer(uint32_t, uint32_t) final;

		const RenderStateDesc &GetMaterialRenderState(const Graphics::Material *m) final;

		const BufferBinding<UniformBuffer> &GetLightUniformBuffer();
		// the shadow state for this frame; allocates a "shadows off" block if none was set
		const BufferBinding<UniformBuffer> &GetShadowUniformBuffer();
		Texture *GetShadowMap(int cascade) const { return m_shadowMaps[cascade]; }
		// the city lights for this frame; allocates a "lights off" block if none was set
		const BufferBinding<UniformBuffer> &GetCityLightUniformBuffer();
		Texture *GetCityLightMap() const { return m_cityLightMap; }
		Texture *GetCityLightGrid() const { return m_cityLightGrid; }
		// the city's streets for this frame; allocates a "no city" block if none was set
		const BufferBinding<UniformBuffer> &GetCityGroundUniformBuffer();
		Texture *GetCityRoads() const { return m_cityRoads; }
		Texture *GetCityLots() const { return m_cityLots; }
		// the ships' lights for this frame; allocates a "lights off" block if none was set
		const BufferBinding<UniformBuffer> &GetShipLightUniformBuffer();
		// the rover's tracks for this frame; allocates a "no tracks" block if none was set
		const BufferBinding<UniformBuffer> &GetSurfaceTrackUniformBuffer();
		Texture *GetSurfaceTracks() const { return m_surfaceTracks; }
		OGL::UniformLinearBuffer *GetDrawUniformBuffer(Uint32 size);
		OGL::RenderStateCache *GetStateCache() { return m_renderStateCache.get(); }

		bool ReloadShaders() final;

		bool Screendump(ScreendumpState &sd) final;

		void RecordDrawStats(PrimitiveType pt, uint32_t numElements, uint32_t numInstances);

	protected:
		void PushState() final{};
		void PopState() final{};

		size_t m_frameNum;

		Uint32 m_numLights;
		Uint32 m_numDirLights;
		float m_minZNear;
		float m_maxZFar;
		bool m_useCompressedTextures;
		bool m_useAnisotropicFiltering;

		// TODO: iterate shaderdef files on startup and cache by Shader name directive rather than filename fragment
		std::vector<std::pair<std::string, OGL::Shader *>> m_shaders;
		std::vector<std::unique_ptr<OGL::UniformLinearBuffer>> m_drawUniformBuffers;
		std::unique_ptr<OGL::RenderStateCache> m_renderStateCache;
		BufferBinding<UniformBuffer> m_lightUniformBuffer;
		BufferBinding<UniformBuffer> m_shadowUniformBuffer;
		Texture *m_shadowMaps[ShadowDataBlock::NUM_CASCADES] = {};
		BufferBinding<UniformBuffer> m_cityLightUniformBuffer;
		Texture *m_cityLightMap = nullptr;
		Texture *m_cityLightGrid = nullptr;
		BufferBinding<UniformBuffer> m_cityGroundUniformBuffer;
		Texture *m_cityRoads = nullptr;
		Texture *m_cityLots = nullptr;
		BufferBinding<UniformBuffer> m_shipLightUniformBuffer;
		BufferBinding<UniformBuffer> m_surfaceTrackUniformBuffer;
		Texture *m_surfaceTracks = nullptr;
		bool m_useNVDepthRanged;
		OGL::RenderTarget *m_activeRenderTarget = nullptr;
		std::unique_ptr<OGL::CommandList> m_drawCommandList;

		// GPU timers: timestamp queries recorded in the command stream and read
		// back a few frames later, so the CPU never waits for the GPU
		struct GpuTimerScope {
			std::string name;
			GLuint beginQuery = 0, endQuery = 0;
			int depth = 0;
		};
		struct GpuTimerFrame {
			std::vector<GpuTimerScope> scopes;
			bool pending = false; // recorded, not yet read back
		};
		static const int NUM_GPU_TIMER_FRAMES = 4;
		GpuTimerFrame m_gpuTimerFrames[NUM_GPU_TIMER_FRAMES];
		int m_gpuTimerCurrent = 0;
		std::vector<int> m_gpuTimerStack; // open scopes, as indices into the current frame
		std::vector<GLuint> m_gpuQueryPool;
		std::vector<GpuTiming> m_gpuTimings;
		int m_gpuTimersSupported = -1; // unknown until first use
		bool GpuTimersSupported();
		GLuint AllocGpuQuery();
		void ResolveGpuTimers();
		void ReadGpuTimerFrame(GpuTimerFrame &frame);

		matrix4x4f m_modelViewMat;
		matrix4x4f m_projectionMat;
		ViewportExtents m_viewport;

	private:
		static bool initted;

		template <typename T>
		struct TempBuffer {
			std::unique_ptr<T> buffer;
			uint32_t flushed = 0;
		};

		template<typename T>
		using TempBufferPool = std::vector<TempBuffer<T>>;

		TempBufferPool<VertexBuffer> m_tempVtxBuffers;

		SDL_GLContext m_glContext;
	};
#define CHECKERRORS() RendererOGL::CheckErrors(__FUNCTION__, __LINE__)

} // namespace Graphics
