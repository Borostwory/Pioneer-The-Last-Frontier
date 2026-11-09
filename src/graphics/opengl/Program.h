// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#pragma once

#include "OpenGLLibs.h"

#include <memory>
#include <string>
#include <vector>

namespace Graphics {

	namespace OGL {

		struct ShaderCompileException {};
		struct ProgramException {};

		struct ProgramDef;
		class Shader;

		/*
		* A Program is a specific, immutable variant of a shader that maps closely to
		* the underlying API's terminology (Program, GraphicsPipeline, etc.)
		*/
		class Program {
		public:
			// background: built by the driver's own threads while the game goes on, where it can
			// (SetBackgroundSupported), to be finished with Poll(); otherwise built before this returns
			Program(Shader *shader, const ProgramDef &def, bool background = false);
			~Program();

			void Reload(Shader *shader, const ProgramDef &def);
			bool Loaded() const { return success; }
			// still being built in the background: not to be drawn with yet
			bool Pending() const { return m_pending != nullptr; }
			// finishes a background build once the driver is done with it (wait: waits for it); true if it's ready
			// to draw with
			bool Poll(bool wait = false);

			// whether the driver builds programs on its own threads (GL_ARB/KHR_parallel_shader_compile), set by
			// the renderer when it starts
			static void SetBackgroundSupported(bool on);
			static bool BackgroundSupported();

			GLuint GetConstantLocation(uint32_t index) const { return m_constants[index]; }
			GLuint GetProgramID() const { return m_program; }

			const Shader *GetShader() const { return m_shader; }

		protected:
			struct Build;
			GLuint LoadShaders(const ProgramDef &def, std::unique_ptr<Build> &build);
			bool FinishBuild(GLuint program, Build &build);
			void InitUniforms(const Shader *shader);

			const Shader *m_shader;
			GLuint m_program;
			bool success;
			// a build handed to the driver and not yet finished (Poll)
			std::unique_ptr<Build> m_pending;

			// map of push constant bindings to glUniform locations
			std::vector<GLuint> m_constants;
		};

	} // namespace OGL

} // namespace Graphics
