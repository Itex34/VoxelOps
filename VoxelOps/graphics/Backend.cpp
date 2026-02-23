#include "Backend.hpp"

#include <glad/glad.h>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
bool hasExtension(const char* extensionName) {
	GLint extensionCount = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);

	for (GLint i = 0; i < extensionCount; ++i) {
		const char* extension = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
		if (extension && std::strcmp(extension, extensionName) == 0) {
			return true;
		}
	}

	return false;
}

std::string_view backendName(GraphicsBackend backend) {
	switch (backend) {
	case GraphicsBackend::Realistic:
		return "Realistic";
	case GraphicsBackend::Performance:
		return "Performance";
	case GraphicsBackend::Potato:
		return "Potato";
	default:
		return "Unknown";
	}
}
}

Backend::Backend() {
	if (!GLAD_GL_VERSION_1_0) {
		std::cerr << "[Backend] GLAD is not initialized. Construct Backend only after gladLoadGLLoader.\n";
		std::abort();
	}

	if (glGetString(GL_VERSION) == nullptr) {
		std::cerr << "[Backend] No active OpenGL context. Construct Backend only after context creation.\n";
		std::abort();
	}

	OpenGLVersionMajor = getMaxSupportedGLVersionMajor();
	OpenGLVersionMinor = getMaxSupportedGLVersionMinor();

	const bool hasCoreMDI = isAtLeast(OpenGLVersionMajor, OpenGLVersionMinor, 4, 3);
	supportsMDI = hasCoreMDI || hasExtension("GL_ARB_multi_draw_indirect");
	const bool hasCoreShaderDrawParameters = isAtLeast(OpenGLVersionMajor, OpenGLVersionMinor, 4, 6);
	supportsShaderDrawParameters = hasCoreShaderDrawParameters || hasExtension("GL_ARB_shader_draw_parameters");

	activeBackend = chooseBackend();
	initialized = true;

	static bool loggedStartupCaps = false;
	if (!loggedStartupCaps) {
		std::cout
			<< "[Backend] OpenGL " << OpenGLVersionMajor << "." << OpenGLVersionMinor
			<< " | MDI: " << (supportsMDI ? "yes" : "no")
			<< " | ShaderDrawParams: " << (supportsShaderDrawParameters ? "yes" : "no")
			<< " | Tier: " << backendName(activeBackend)
			<< "\n";
		loggedStartupCaps = true;
	}
}

int Backend::getMaxSupportedGLVersionMajor() const {
	GLint major = 0;
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	return static_cast<int>(major);
}

int Backend::getMaxSupportedGLVersionMinor() const {
	GLint minor = 0;
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	return static_cast<int>(minor);
}

GraphicsBackend Backend::chooseBackend() const {
	if (isMDIUsable()) {
		return GraphicsBackend::Realistic;
	}

	if (isAtLeast(OpenGLVersionMajor, OpenGLVersionMinor, 3, 3)) {
		return GraphicsBackend::Performance;
	}

	return GraphicsBackend::Potato;
}

bool Backend::isAtLeast(int major, int minor, int requiredMajor, int requiredMinor) noexcept {
	if (major > requiredMajor) {
		return true;
	}
	if (major < requiredMajor) {
		return false;
	}
	return minor >= requiredMinor;
}

int Backend::getOpenGLVersionMajor() const noexcept {
	return OpenGLVersionMajor;
}

int Backend::getOpenGLVersionMinor() const noexcept {
	return OpenGLVersionMinor;
}

bool Backend::isMDISupported() const noexcept {
	return supportsMDI;
}

bool Backend::isShaderDrawParametersSupported() const noexcept {
	return supportsShaderDrawParameters;
}

bool Backend::isMDIUsable() const noexcept {
	return supportsMDI && supportsShaderDrawParameters;
}

GraphicsBackend Backend::getActiveBackend() const noexcept {
	return activeBackend;
}

std::string_view Backend::getActiveBackendName() const noexcept {
	return backendName(activeBackend);
}

bool Backend::isInitialized() const noexcept {
	return initialized;
}
