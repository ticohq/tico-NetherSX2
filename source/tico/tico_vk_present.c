#include <stdlib.h>
#include <string.h>

#include "tico/overlay_hook.h"
#include "tico/tico_entry.h"
#include "tico/tico_vk_present.h"

#define MAX_IMAGES 8

static struct {
  VkDevice device;
  VkSwapchainKHR swapchain;
  TicoVkSwapchainInfo info;
  VkRenderPass render_pass;
  VkCommandPool command_pool;
  uint32_t image_count;
  VkImageView views[MAX_IMAGES];
  VkFramebuffer framebuffers[MAX_IMAGES];
  VkCommandBuffer commands[MAX_IMAGES];
  VkFence fences[MAX_IMAGES];
  VkSemaphore done[MAX_IMAGES];
  int failed; /* setup failed for this swapchain: do not retry every frame */
} s;

void tico_vk_overlay_reset(void) {
  if (!s.device) return;
  vkDeviceWaitIdle(s.device);
  for (uint32_t i = 0; i < s.image_count; i++) {
    if (s.done[i]) vkDestroySemaphore(s.device, s.done[i], NULL);
    if (s.fences[i]) vkDestroyFence(s.device, s.fences[i], NULL);
    if (s.framebuffers[i]) vkDestroyFramebuffer(s.device, s.framebuffers[i], NULL);
    if (s.views[i]) vkDestroyImageView(s.device, s.views[i], NULL);
  }
  if (s.command_pool) vkDestroyCommandPool(s.device, s.command_pool, NULL);
  if (s.render_pass) vkDestroyRenderPass(s.device, s.render_pass, NULL);
  memset(&s, 0, sizeof(s));
}

static int setup(const TicoVkSwapchainInfo *info) {
  tico_vk_overlay_reset();
  s.device = info->device;
  s.swapchain = info->swapchain;
  s.info = *info;

  VkImage images[MAX_IMAGES];
  uint32_t count = MAX_IMAGES;
  VkResult result = vkGetSwapchainImagesKHR(s.device, s.swapchain, &count, images);
  if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !count) return 0;
  s.image_count = count;

  /* the image keeps what the core drew: load it, and leave it ready to present */
  const VkAttachmentDescription attachment = {
    .format = info->format,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    .initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
  };
  const VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkSubpassDescription subpass = {
    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
    .colorAttachmentCount = 1,
    .pColorAttachments = &color,
  };
  const VkSubpassDependency dependency = {
    .srcSubpass = VK_SUBPASS_EXTERNAL,
    .dstSubpass = 0,
    .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
  };
  const VkRenderPassCreateInfo pass_info = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments = &attachment,
    .subpassCount = 1,
    .pSubpasses = &subpass,
    .dependencyCount = 1,
    .pDependencies = &dependency,
  };
  if (vkCreateRenderPass(s.device, &pass_info, NULL, &s.render_pass) != VK_SUCCESS)
    return 0;

  const VkCommandPoolCreateInfo pool_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    .queueFamilyIndex = info->queue_family,
  };
  if (vkCreateCommandPool(s.device, &pool_info, NULL, &s.command_pool) != VK_SUCCESS)
    return 0;
  const VkCommandBufferAllocateInfo command_info = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = s.command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = count,
  };
  if (vkAllocateCommandBuffers(s.device, &command_info, s.commands) != VK_SUCCESS)
    return 0;

  for (uint32_t i = 0; i < count; i++) {
    const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = images[i],
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = info->format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    if (vkCreateImageView(s.device, &view_info, NULL, &s.views[i]) != VK_SUCCESS)
      return 0;
    const VkFramebufferCreateInfo framebuffer_info = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = s.render_pass,
      .attachmentCount = 1,
      .pAttachments = &s.views[i],
      .width = info->extent.width,
      .height = info->extent.height,
      .layers = 1,
    };
    if (vkCreateFramebuffer(s.device, &framebuffer_info, NULL, &s.framebuffers[i]) != VK_SUCCESS)
      return 0;
    const VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    const VkSemaphoreCreateInfo semaphore_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    if (vkCreateFence(s.device, &fence_info, NULL, &s.fences[i]) != VK_SUCCESS ||
        vkCreateSemaphore(s.device, &semaphore_info, NULL, &s.done[i]) != VK_SUCCESS)
      return 0;
  }
  tico_log("overlay: Vulkan present path ready (%u images, %ux%u, format %d)\n", count,
           info->extent.width, info->extent.height, (int)info->format);
  return 1;
}

int tico_vk_overlay_present(VkQueue queue, const VkPresentInfoKHR *present,
                            const TicoVkSwapchainInfo *swapchain,
                            VkPresentInfoKHR *out) {
  if (!tico_vk_overlay_hook || !tico_overlay_wants_frame || !tico_overlay_wants_frame())
    return 0;
  if (!present || present->swapchainCount != 1 || !present->pImageIndices ||
      present->pSwapchains[0] != swapchain->swapchain)
    return 0;
  if (s.swapchain != swapchain->swapchain) {
    s.failed = !setup(swapchain);
    if (s.failed) {
      tico_log("overlay: Vulkan present path setup failed\n");
      VkSwapchainKHR failed_swapchain = swapchain->swapchain;
      tico_vk_overlay_reset();
      s.swapchain = failed_swapchain;
      s.failed = 1;
    }
  }
  if (s.failed) return 0;
  const uint32_t index = present->pImageIndices[0];
  if (index >= s.image_count) return 0;

  vkWaitForFences(s.device, 1, &s.fences[index], VK_TRUE, UINT64_MAX);
  vkResetFences(s.device, 1, &s.fences[index]);
  VkCommandBuffer command = s.commands[index];
  vkResetCommandBuffer(command, 0);
  const VkCommandBufferBeginInfo begin = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkBeginCommandBuffer(command, &begin);
  const VkRenderPassBeginInfo pass = {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
    .renderPass = s.render_pass,
    .framebuffer = s.framebuffers[index],
    .renderArea = {{0, 0}, s.info.extent},
  };
  vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
  const TicoVkOverlayContext context = {
    .instance = s.info.instance,
    .physical_device = s.info.physical_device,
    .device = s.device,
    .queue = queue,
    .queue_family = s.info.queue_family,
    .render_pass = s.render_pass,
    .command_buffer = command,
    .format = s.info.format,
    .image_count = s.image_count,
    .width = s.info.extent.width,
    .height = s.info.extent.height,
  };
  tico_vk_overlay_hook(&context);
  vkCmdEndRenderPass(command);
  vkEndCommandBuffer(command);

  VkPipelineStageFlags stages[MAX_IMAGES];
  uint32_t waits = present->waitSemaphoreCount;
  if (waits > MAX_IMAGES) waits = MAX_IMAGES;
  for (uint32_t i = 0; i < waits; i++)
    stages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  const VkSubmitInfo submit = {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .waitSemaphoreCount = waits,
    .pWaitSemaphores = present->pWaitSemaphores,
    .pWaitDstStageMask = stages,
    .commandBufferCount = 1,
    .pCommandBuffers = &command,
    .signalSemaphoreCount = 1,
    .pSignalSemaphores = &s.done[index],
  };
  if (vkQueueSubmit(queue, 1, &submit, s.fences[index]) != VK_SUCCESS) {
    /* the fence stays unsignalled: make a fresh one so the next wait returns */
    vkDestroyFence(s.device, s.fences[index], NULL);
    const VkFenceCreateInfo fence_info = {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    vkCreateFence(s.device, &fence_info, NULL, &s.fences[index]);
    return 0;
  }
  *out = *present;
  out->waitSemaphoreCount = 1;
  out->pWaitSemaphores = &s.done[index];
  return 1;
}
