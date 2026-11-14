#pragma once

// The renderer cannot continue after a failed Vulkan call or a broken invariant, and it used
// to call abort() without a word: after a lost device the only trace was the kernel's log.
// Every such exit now goes through here and first writes one line to stderr (the launcher's
// log) that names the call, the result and the place.

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>

namespace Indium {
	inline const char* vulkanResultName(VkResult result) {
		switch (result) {
#define INDIUM_RESULT_NAME(name) case name: return #name;
			INDIUM_RESULT_NAME(VK_SUCCESS)
			INDIUM_RESULT_NAME(VK_NOT_READY)
			INDIUM_RESULT_NAME(VK_TIMEOUT)
			INDIUM_RESULT_NAME(VK_EVENT_SET)
			INDIUM_RESULT_NAME(VK_EVENT_RESET)
			INDIUM_RESULT_NAME(VK_INCOMPLETE)
			INDIUM_RESULT_NAME(VK_ERROR_OUT_OF_HOST_MEMORY)
			INDIUM_RESULT_NAME(VK_ERROR_OUT_OF_DEVICE_MEMORY)
			INDIUM_RESULT_NAME(VK_ERROR_INITIALIZATION_FAILED)
			INDIUM_RESULT_NAME(VK_ERROR_DEVICE_LOST)
			INDIUM_RESULT_NAME(VK_ERROR_MEMORY_MAP_FAILED)
			INDIUM_RESULT_NAME(VK_ERROR_LAYER_NOT_PRESENT)
			INDIUM_RESULT_NAME(VK_ERROR_EXTENSION_NOT_PRESENT)
			INDIUM_RESULT_NAME(VK_ERROR_FEATURE_NOT_PRESENT)
			INDIUM_RESULT_NAME(VK_ERROR_INCOMPATIBLE_DRIVER)
			INDIUM_RESULT_NAME(VK_ERROR_TOO_MANY_OBJECTS)
			INDIUM_RESULT_NAME(VK_ERROR_FORMAT_NOT_SUPPORTED)
			INDIUM_RESULT_NAME(VK_ERROR_FRAGMENTED_POOL)
			INDIUM_RESULT_NAME(VK_ERROR_UNKNOWN)
			INDIUM_RESULT_NAME(VK_ERROR_OUT_OF_POOL_MEMORY)
			INDIUM_RESULT_NAME(VK_ERROR_INVALID_EXTERNAL_HANDLE)
			INDIUM_RESULT_NAME(VK_ERROR_FRAGMENTATION)
			INDIUM_RESULT_NAME(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
			INDIUM_RESULT_NAME(VK_PIPELINE_COMPILE_REQUIRED)
			INDIUM_RESULT_NAME(VK_ERROR_SURFACE_LOST_KHR)
			INDIUM_RESULT_NAME(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
			INDIUM_RESULT_NAME(VK_SUBOPTIMAL_KHR)
			INDIUM_RESULT_NAME(VK_ERROR_OUT_OF_DATE_KHR)
#undef INDIUM_RESULT_NAME
			default: return "unknown VkResult";
		}
	};

	[[noreturn]] inline void vulkanFatal(const char* call, VkResult result, const char* file, int line) {
		std::fprintf(stderr, "Indium: fatal: %s failed: %s (%d) at %s:%d\n", call, vulkanResultName(result), int(result), file, line);
		std::fflush(stderr);
		std::abort();
	};

	[[noreturn]] inline void fatal(const char* reason, const char* file, int line) {
		std::fprintf(stderr, "Indium: fatal: %s at %s:%d\n", reason, file, line);
		std::fflush(stderr);
		std::abort();
	};
};

// __FILE_NAME__ is the file's last path component: no build directory ends up in the library.
#define INDIUM_VULKAN_FATAL(call, result) ::Indium::vulkanFatal(call, result, __FILE_NAME__, __LINE__)
#define INDIUM_FATAL(reason) ::Indium::fatal(reason, __FILE_NAME__, __LINE__)
