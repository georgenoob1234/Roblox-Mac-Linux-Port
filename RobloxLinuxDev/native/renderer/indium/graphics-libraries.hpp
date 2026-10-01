#pragma once
#include <indium/device.private.hpp>
#include <indium/library.private.hpp>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

namespace Indium {
// One pre-rasterization or fragment-shader pipeline library per shader
// module, shared by every pipeline state that uses the function, so a new
// material that reuses loaded shaders only pays a fast link.
struct ShaderLibrary {
	VkDevice device = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE; // null after a failed or cancelled build
	std::once_flag once;
	~ShaderLibrary();
};

class GraphicsLibraries: public std::enable_shared_from_this<GraphicsLibraries> {
	PrivateDevice& _device;
	std::mutex _mutex;
	std::condition_variable _idle;
	std::map<std::pair<VkShaderModule, int>, std::shared_ptr<ShaderLibrary>> _libraries;
	unsigned _running = 0;
	bool _closed = false;
	std::shared_ptr<ShaderLibrary> _emptyFragment;
	std::shared_ptr<ShaderLibrary> entry(VkShaderModule module, FunctionType stage);
	void compile(ShaderLibrary& library, VkShaderModule module, const FunctionInfo* info, FunctionType stage);
public:
	explicit GraphicsLibraries(PrivateDevice& device): _device(device) {}
	// The built library for a function, compiling it on this thread unless
	// another thread is already doing so. Its pipeline is null on failure.
	std::shared_ptr<ShaderLibrary> get(VkShaderModule module, const FunctionInfo& info);
	// Fragment-shader state for depth-only pipelines.
	std::shared_ptr<ShaderLibrary> emptyFragment();
	// Queues a low-priority build when a shader library is loaded.
	void prepare(VkShaderModule module, const FunctionInfo& info);
	// Before a shader module is destroyed: waits out a running build.
	void release(VkShaderModule module);
	// Before the device is destroyed: no queued build may start afterwards.
	void close();
};
}
