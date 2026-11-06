// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "Program.h"
#include "FileSystem.h"
#include "Shader.h"
#include "StringF.h"
#include "StringRange.h"
#include "core/FNV1a.h"
#include "graphics/Graphics.h"
#include "graphics/Renderer.h"
#include "graphics/Types.h"
#include "utils.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

namespace Graphics {

	namespace OGL {

		// #version 140 for OpenGL3.1
		// #version 150 for OpenGL3.2
		// #version 330 for OpenGL3.3
		static const char *s_glslVersion = "#version 140\n";

		// Check and warn about compile & link errors
		static bool check_glsl_errors(const char *filename, GLuint obj)
		{
			//check if shader or program
			bool isShader = (glIsShader(obj) == GL_TRUE);

			int infologLength = 0;
			char infoLog[1024];

			if (isShader)
				glGetShaderInfoLog(obj, 1024, &infologLength, infoLog);
			else
				glGetProgramInfoLog(obj, 1024, &infologLength, infoLog);

			GLint status;
			if (isShader)
				glGetShaderiv(obj, GL_COMPILE_STATUS, &status);
			else
				glGetProgramiv(obj, GL_LINK_STATUS, &status);

			if (status == GL_FALSE) {
				Log::Error("Error compiling shader: {}:\n {}\n OpenGL vendor: {}\nOpenGL renderer string: {}",
					filename, infoLog, glstr_to_str(glGetString(GL_VENDOR)), glstr_to_str(glGetString(GL_RENDERER)));
				return false;
			}

#if 0
	if (!isShader) {
		// perform general validation that the program is usable
		glValidateProgram(obj);

		glGetProgramiv(obj, GL_VALIDATE_STATUS, &status);

		if (status == GL_FALSE) {
			Error("Error vaildating shader: %s:\n%sOpenGL vendor: %s\nOpenGL renderer string: %s",
				filename, infoLog, glGetString(GL_VENDOR), glGetString(GL_RENDERER));
			return false;
		}
	}
#endif

			// TODO: this code is obsolete
			// Log warnings even if successfully compiled
			// Sometimes the log is full of junk "success" messages so
			// this is not a good use for OS::Warning
#ifndef NDEBUG
			if (infologLength > 0) {
				if (pi_strcasestr(infoLog, "warning"))
					Output("%s: %s", filename, infoLog);
			}
#endif

			return true;
		}

		// ShaderProgram is a helper class to wrap fragment/vertex shader
		// creation. The hierarchy is conceptually:
		//   Shader ->
		//     Program ->
		//       ShaderProgram (vertex)
		//       ShaderProgram (fragment)
		struct ShaderProgram {
			// reads the source; Compile() makes the shader, which a program from the binary cache doesn't need
			ShaderProgram(GLenum type, const std::string &filename, const std::string &defines) :
				m_type(type),
				m_filename(filename),
				m_defines(defines)
			{
				RefCountedPtr<FileSystem::FileData> filecode = FileSystem::gameDataFiles.ReadFile(filename);

				if (!filecode.Valid())
					Error("Could not load %s", filename.c_str());

				std::string &strCode = m_code;
				strCode = filecode->AsStringRange().ToString();
				size_t found = strCode.find("#include");
				while (found != std::string::npos) {
					// find the name of the file to include
					const size_t begFilename = strCode.find_first_of("\"", found + 8) + 1;
					const size_t endFilename = strCode.find_first_of("\"", begFilename + 1);

					const std::string incFilename = strCode.substr(begFilename, endFilename - begFilename);

					// check we haven't it already included it (avoids circular dependencies)
					const std::set<std::string>::const_iterator foundIt = previousIncludes.find(incFilename);
					if (foundIt != previousIncludes.end()) {
						Error("Circular, or multiple, include of %s\n", incFilename.c_str());
					} else {
						previousIncludes.insert(incFilename);
					}

					// build path for include
					const std::string incPathBuffer = stringf("shaders/opengl/%0", incFilename);

					// read included file
					RefCountedPtr<FileSystem::FileData> incCode = FileSystem::gameDataFiles.ReadFile(incPathBuffer);
					assert(incCode.Valid());

					if (incCode.Valid()) {
						// replace the #include and filename with the included files text
						strCode.replace(found, (endFilename + 1) - found, incCode->GetData(), incCode->GetSize());
						found = strCode.find("#include");
					} else {
						Error("Could not load shader #include %s for shader %s\n", incPathBuffer.c_str(), filename.c_str());
					}
				}
			}

			// the source with the included files in it
			const std::string &GetCode() const { return m_code; }

			void Compile()
			{
				const GLenum type = m_type;
				const std::string &filename = m_filename;

				// Store the modified text with the included files (if any)
				const StringRange code(m_code.c_str(), m_code.size());

				// Build the final shader text to be compiled
				AppendSource(s_glslVersion);
				AppendSource(m_defines.c_str());
				if (type == GL_VERTEX_SHADER) {
					AppendSource("#define VERTEX_SHADER\n");
				} else {
					AppendSource("#define FRAGMENT_SHADER\n");
				}
				AppendSource(code.StripUTF8BOM());
#if 0
		static bool s_bDumpShaderSource = true;
		if (s_bDumpShaderSource) {
			const char SHADER_OUT_DIR_NAME[] = "shaders";
			const char SHADER_OGL_OUT_DIR_NAME[] = "shaders/opengl";
			FileSystem::userFiles.MakeDirectory(SHADER_OUT_DIR_NAME);
			FileSystem::userFiles.MakeDirectory(SHADER_OGL_OUT_DIR_NAME);
			const std::string outFilename(FileSystem::GetUserDir() + "/" + filename);
			FILE *tmp = fopen(outFilename.c_str(), "wb");
			if(tmp) {
				Output("%s", filename);
				for( Uint32 i=0; i<blocks.size(); i++ ) {
					const char *block = blocks[i];
					const GLint sizes = block_sizes[i];
					if(block && sizes>0) {
						fprintf(tmp, "%.*s", sizes, block);
					}
				}
				fclose(tmp);
			} else {
				Output("Could not open file %s", outFilename.c_str());
			}
		}
#endif
				shader = glCreateShader(type);
				if (glIsShader(shader) != GL_TRUE)
					throw ShaderCompileException();

				Compile(shader);

				if (!check_glsl_errors(filename.c_str(), shader)) {
					glDeleteShader(shader);
					shader = 0;
				}
			}

			~ShaderProgram()
			{
				if (shader)
					glDeleteShader(shader);
			}

			GLuint shader = 0;

		private:
			void AppendSource(const char *str)
			{
				blocks.push_back(str);
				block_sizes.push_back(std::strlen(str));
			}

			void AppendSource(StringRange str)
			{
				blocks.push_back(str.begin);
				block_sizes.push_back(str.Size());
			}

			void Compile(GLuint shader_id)
			{
				assert(blocks.size() == block_sizes.size());
				glShaderSource(shader_id, blocks.size(), &blocks[0], &block_sizes[0]);
				glCompileShader(shader_id);
			}

			GLenum m_type;
			std::string m_filename;
			std::string m_defines;
			std::string m_code;
			std::vector<const char *> blocks;
			std::vector<GLint> block_sizes;
			std::set<std::string> previousIncludes;
		};

		// ====================================================================
		// Binary Cache
		//
		// A program can take seconds to compile, and the driver's own cache can't be relied on to keep it (it has a size
		// limit, and may be full). So each one's compiled form is kept in the user folder, a file for each shader and
		// defines. The file says which source and driver it's from: from any other, it's compiled again and replaced.
		//

		static const char s_binaryCacheDir[] = "shadercache";
		static const char s_binaryMagic[8] = { 'T', 'L', 'F', 'P', 'R', 'O', 'G', '1' };

		struct BinaryHeader {
			char magic[8];
			uint64_t sourceHash; // of the source, the driver and what LoadShaders sets before linking
			uint64_t dataHash;
			uint32_t format;
			uint32_t size;
		};
		static_assert(sizeof(BinaryHeader) == 32, "BinaryHeader is written to files");

		static uint64_t HashString(const std::string &text)
		{
			return hash_64_fnv1a(text.data(), text.size());
		}

		static bool BinaryCacheSupported()
		{
			static const bool supported = []() {
				if (!glewIsSupported("GL_ARB_get_program_binary"))
					return false;
				GLint formats = 0;
				glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &formats);
				return formats > 0;
			}();
			return supported;
		}

		static std::string BinaryCachePath(const ProgramDef &def)
		{
			char hash[17];
			std::snprintf(hash, sizeof(hash), "%016llx", static_cast<unsigned long long>(HashString(def.defines)));
			return std::string(s_binaryCacheDir) + "/" + def.name + "-" + hash + ".bin";
		}

		static uint64_t BinarySourceHash(const ProgramDef &def, const ShaderProgram &vs, const ShaderProgram &fs)
		{
			std::string text = "1\n"; // (raise this if LoadShaders changes what it sets before linking)
			for (const GLenum name : { GL_VENDOR, GL_RENDERER, GL_VERSION }) {
				text += glstr_to_str(glGetString(name));
				text += "\n";
			}
			text += s_glslVersion;
			text += def.defines;
			text += vs.GetCode();
			text += fs.GetCode();
			return HashString(text);
		}

		// returns the linked program, or 0 if there's no good one to be had
		static GLuint LoadBinary(const std::string &path, const uint64_t sourceHash)
		{
			RefCountedPtr<FileSystem::FileData> file = FileSystem::userFiles.ReadFile(path);
			if (!file.Valid() || file->GetSize() < sizeof(BinaryHeader))
				return 0;

			BinaryHeader header;
			std::memcpy(&header, file->GetData(), sizeof(header));
			const char *data = file->GetData() + sizeof(header);
			if (std::memcmp(header.magic, s_binaryMagic, sizeof(s_binaryMagic)) != 0 || header.sourceHash != sourceHash)
				return 0;
			// (a partly written or damaged file must not reach the driver)
			if (header.size != file->GetSize() - sizeof(header) || header.dataHash != hash_64_fnv1a(data, header.size))
				return 0;

			GLuint program = glCreateProgram();
			glProgramBinary(program, header.format, data, header.size);
			GLint status = GL_FALSE;
			glGetProgramiv(program, GL_LINK_STATUS, &status);
			if (status == GL_FALSE) {
				// a driver may refuse a binary at any time: not an error, it's compiled instead
				glDeleteProgram(program);
				glGetError();
				return 0;
			}
			return program;
		}

		static void SaveBinary(const std::string &path, const uint64_t sourceHash, const GLuint program)
		{
			GLint length = 0;
			glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
			if (length <= 0)
				return;

			std::vector<char> data(length);
			GLsizei written = 0;
			GLenum format = 0;
			glGetProgramBinary(program, length, &written, &format, data.data());
			if (written <= 0)
				return;

			BinaryHeader header;
			std::memcpy(header.magic, s_binaryMagic, sizeof(s_binaryMagic));
			header.sourceHash = sourceHash;
			header.dataHash = hash_64_fnv1a(data.data(), size_t(written));
			header.format = format;
			header.size = uint32_t(written);

			FileSystem::userFiles.MakeDirectory(s_binaryCacheDir);
			FILE *file = FileSystem::userFiles.OpenWriteStream(path);
			if (!file)
				return;
			const bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1 && std::fwrite(data.data(), 1, size_t(written), file) == size_t(written);
			std::fclose(file);
			if (!ok)
				FileSystem::userFiles.RemoveFile(path);
		}

		// ====================================================================
		// Program Setup and Creation
		//

		Program::Program(Shader *shader, const ProgramDef &def) :
			m_shader(shader),
			m_program(0),
			success(false)
		{
			m_program = LoadShaders(def);
			if (success)
				InitUniforms(shader);
		}

		Program::~Program()
		{
			if (m_program)
				glDeleteProgram(m_program);
		}

		void Program::Reload(Shader *shader, const ProgramDef &def)
		{
			GLuint newProg = LoadShaders(def);
			if (newProg) {
				glDeleteProgram(m_program);
				m_program = newProg;
				InitUniforms(shader);
			}
		}

		//load, compile and link
		GLuint Program::LoadShaders(const ProgramDef &def)
		{
			PROFILE_SCOPED()

			const auto startTime = std::chrono::steady_clock::now();
			const auto millisecondsSoFar = [&startTime]() {
				return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
			};

			//load the shaders' source
			ShaderProgram vs(GL_VERTEX_SHADER, def.vertexShader, def.defines);
			ShaderProgram fs(GL_FRAGMENT_SHADER, def.fragmentShader, def.defines);

			// the compiled program from an earlier run, if the source and the driver are the same
			const bool useBinaryCache = BinaryCacheSupported();
			std::string binaryPath;
			uint64_t binarySourceHash = 0;
			if (useBinaryCache) {
				binaryPath = BinaryCachePath(def);
				binarySourceHash = BinarySourceHash(def, vs, fs);
				const GLuint cached = LoadBinary(binaryPath, binarySourceHash);
				if (cached) {
					Log::Info("Program {} loaded from the binary cache in {:.1f} ms", def.name, millisecondsSoFar());
					success = true;
					return cached;
				}
			}

			//create and compile shaders
			vs.Compile();
			fs.Compile();

			if (!vs.shader || !fs.shader) {
				Log::Warning("Error loading GLSL shaders for program {}\n", def.name);
				success = false;
				return 0;
			}

			//create program, attach shaders and link
			GLuint program = glCreateProgram();
			if (glIsProgram(program) != GL_TRUE)
				throw ProgramException();

			glAttachShader(program, vs.shader);
			glAttachShader(program, fs.shader);

			//extra attribs, if they exist
			glBindAttribLocation(program, 0, "a_vertex");
			glBindAttribLocation(program, 1, "a_normal");
			glBindAttribLocation(program, 2, "a_color");
			glBindAttribLocation(program, 3, "a_uv0");
			glBindAttribLocation(program, 4, "a_uv1");
			glBindAttribLocation(program, 5, "a_tangent");
			glBindAttribLocation(program, 6, "a_transform");
			// a_transform @ 6 shadows (uses) 7, 8, and 9
			// next available is layout (location = 10)

			// TODO: setup fragment output locations from shaderdef attributes
			glBindFragDataLocation(program, 0, "frag_color");

			if (useBinaryCache)
				glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);

			glLinkProgram(program);
			success = check_glsl_errors(def.name.c_str(), program);

			if (!success) {
				glDeleteProgram(program);
				return 0;
			}

			Log::Info("Program {} compiled in {:.1f} ms", def.name, millisecondsSoFar());

			if (useBinaryCache)
				SaveBinary(binaryPath, binarySourceHash, program);

			//shaders may now be deleted by Shader destructor
			return program;
		}

		void Program::InitUniforms(Shader *shader)
		{
			glUseProgram(m_program);

			// Bind texture sampler locations to texture units
			for (auto &texInfo : shader->GetTextureBindings()) {
				GLuint location = glGetUniformLocation(m_program, shader->GetString(texInfo.bindName).c_str());
				if (location == GL_INVALID_INDEX)
					continue;

				glUniform1i(location, texInfo.binding);
			}

			// Bind uniform buffer blocks to buffer binding points
			for (auto &bufInfo : shader->GetBufferBindings()) {
				GLuint location = glGetUniformBlockIndex(m_program, shader->GetString(bufInfo.name).c_str());
				if (location == GL_INVALID_INDEX)
					continue;

				glUniformBlockBinding(m_program, location, bufInfo.binding);
			}

			// Build the table of active push constants and their uniform locations
			m_constants.resize(shader->GetNumPushConstants());
			for (auto &conInfo : shader->GetPushConstantBindings()) {
				GLuint location = glGetUniformLocation(m_program, shader->GetString(conInfo.name).c_str());
				m_constants[conInfo.index] = location;
			}

			glUseProgram(0);
		}

	} // namespace OGL

} // namespace Graphics
