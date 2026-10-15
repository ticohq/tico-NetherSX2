/* Draws the tico overlay over a frame the core presents with Vulkan.
 *
 * The core hands its finished swapchain image to vkQueuePresentKHR; before
 * that reaches the driver, the overlay is drawn onto the same image in a
 * render pass of its own (load, not clear), submitted on the core's present
 * queue after the core's work, and the present then waits for it. */
#ifndef NETHERSX2_TICO_VK_PRESENT_H
#define NETHERSX2_TICO_VK_PRESENT_H

#include <vulkan/vulkan.h>

typedef struct {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkSwapchainKHR swapchain;
  VkFormat format;
  VkExtent2D extent;
  uint32_t queue_family;
} TicoVkSwapchainInfo;

/* When the overlay has something to draw, records and submits it for the
 * presented image and fills *out with a copy of *present that waits for it.
 * Returns 0 (and leaves *out alone) when nothing was drawn. */
int tico_vk_overlay_present(VkQueue queue, const VkPresentInfoKHR *present,
                            const TicoVkSwapchainInfo *swapchain,
                            VkPresentInfoKHR *out);

/* Frees what was made for the swapchain; call before it is destroyed. */
void tico_vk_overlay_reset(void);

#endif
