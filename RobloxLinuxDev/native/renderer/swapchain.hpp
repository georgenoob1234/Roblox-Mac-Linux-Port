#pragma once
#include <indium/device.private.hpp>
#include <indium/texture.private.hpp>
#include <array>
#include <functional>
#include <mutex>
#include <vector>

namespace Indium {
// Presents drawables through a Vulkan swapchain: one copy per frame on the
// device's present queue. Replaces the GL import, the compositor pass and the
// EGL swap that used to run inside every Metal commit.
class SwapchainPresenter {
public:
	// Lost: the copy ran (released still runs) but the surface cannot present.
	enum class Result { Presented, Dropped, Failed, Lost };
	// Takes ownership of the surface. Throws if the device cannot present to it.
	SwapchainPresenter(std::shared_ptr<PrivateDevice> device, VkSurfaceKHR surface);
	~SwapchainPresenter();
	// Copies the texture into the next image, scaled to extent, and presents
	// it. On Presented or Lost, `released` runs on the device's event loop once
	// the copy has read the texture; otherwise it never runs and nothing reads it.
	Result present(PrivateTexture& texture, VkExtent2D extent, bool vsync, std::function<void()> released);
	VkPresentModeKHR presentMode() const { return _mode; }
	VkExtent2D extent() const { return _extent; }
private:
	struct Frame {
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer commands = VK_NULL_HANDLE;
		VkSemaphore acquired = VK_NULL_HANDLE;
		uint64_t done = 0; // timeline value of this slot's last copy
	};
	bool recreate(VkExtent2D extent, bool vsync);
	void waitForCopies(uint64_t value);
	void waitIdle();
	std::shared_ptr<PrivateDevice> _device;
	VkSurfaceKHR _surface;
	VkQueue _queue;
	bool _sharedQueue; // no separate present queue: lock around every use
	VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
	VkExtent2D _extent {};
	bool _vsync = false, _stale = false;
	VkFormat _format = VK_FORMAT_UNDEFINED;
	VkPresentModeKHR _mode = VK_PRESENT_MODE_FIFO_KHR;
	std::vector<VkImage> _images;
	std::vector<VkSemaphore> _rendered; // per image, waited by its present
	std::array<Frame, 4> _frames {};
	size_t _next = 0;
	TimelineSemaphore _copies;
	uint64_t _submitted = 0;
	std::mutex _mutex;
};
}
