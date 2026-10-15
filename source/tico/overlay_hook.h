/* Lets the tico overlay draw over each frame the core presents, with
 * whichever renderer is active (after tico-drasticDS's drastic_overlay_hook.h).
 * Both hooks run on the core's presenting thread. */
#ifndef NETHERSX2_TICO_OVERLAY_HOOK_H
#define NETHERSX2_TICO_OVERLAY_HOOK_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family;
  /* the overlay render pass, open on command_buffer when the hook runs */
  VkRenderPass render_pass;
  VkCommandBuffer command_buffer;
  VkFormat format;
  uint32_t image_count;
  uint32_t width;
  uint32_t height;
} TicoVkOverlayContext;

typedef void (*TicoVkOverlayHook)(const TicoVkOverlayContext *context);
/* called with the window framebuffer bound, before the buffer swap */
typedef void (*TicoGlOverlayHook)(int width, int height);
/* whether the hook has anything to draw this frame */
typedef int (*TicoOverlayWantsFrame)(void);

extern TicoVkOverlayHook tico_vk_overlay_hook;
extern TicoGlOverlayHook tico_gl_overlay_hook;
extern TicoOverlayWantsFrame tico_overlay_wants_frame;

/* Set while the menu is open: the presenting thread then stays in the
 * present, drawing the menu over a frozen copy of the game's last frame,
 * which stalls the core until the menu closes (the core's own pause stops
 * its presents, which would freeze the menu with it). */
extern volatile int tico_overlay_hold;

/* The frozen frame drawn under the menu (an ImGui texture id: a GL texture,
 * or a Vulkan descriptor set from tico_overlay_vk_add_texture); 0 clears. */
void tico_overlay_set_background(unsigned long long texture, float u0, float v0, float u1,
                                 float v1);
unsigned long long tico_overlay_vk_add_texture(VkSampler sampler, VkImageView view);
void tico_overlay_vk_remove_texture(unsigned long long texture);

#ifdef __cplusplus
}
#endif

#endif
