/* Draws the tico overlay over a frame the core presents with Vulkan.
 *
 * The core hands its finished swapchain image to vkQueuePresentKHR; before
 * that reaches the driver, the overlay is drawn onto the same image in a
 * render pass of its own (load, not clear), submitted on the core's present
 * queue after the core's work, and the present then waits for it.
 *
 * While the menu is open (tico_overlay_hold) the presenting thread stays in
 * the present instead: the image is copied as the menu's frozen background,
 * then images are acquired, drawn and presented here until the menu closes. */
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
  /* the swapchain's images can be copied from (TRANSFER_SRC): needed to hold */
  int copyable;
} TicoVkSwapchainInfo;

typedef VkResult (*TicoVkPresentFn)(VkQueue queue, const VkPresentInfoKHR *present);

/* When the overlay has something to draw, records and submits it for the
 * presented image and fills *out with a copy of *present that waits for it.
 * Returns 0 (and leaves *out alone) when nothing was drawn. */
int tico_vk_overlay_present(VkQueue queue, const VkPresentInfoKHR *present,
                            const TicoVkSwapchainInfo *swapchain,
                            VkPresentInfoKHR *out);

/* Holds the presenting thread while the menu is open (see above), presenting
 * with present_fn. Returns 0 when it could not (nothing presented), else 1
 * with the first present's result in *result. */
int tico_vk_overlay_hold(VkQueue queue, const VkPresentInfoKHR *present,
                         const TicoVkSwapchainInfo *swapchain, TicoVkPresentFn present_fn,
                         VkResult *result);

/* Frees what was made for the swapchain; call before it is destroyed. */
void tico_vk_overlay_reset(void);

#endif
