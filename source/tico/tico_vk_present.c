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
  VkImage images[MAX_IMAGES];
  VkImageView views[MAX_IMAGES];
  VkFramebuffer framebuffers[MAX_IMAGES];
  VkCommandBuffer commands[MAX_IMAGES];
  VkFence fences[MAX_IMAGES];
  VkSemaphore done[MAX_IMAGES];
  /* the frozen frame under the menu while holding */
  VkImage frozen;
  VkDeviceMemory frozen_memory;
  VkImageView frozen_view;
  VkSampler sampler;
  VkSemaphore acquired;
  int failed; /* setup failed for this swapchain: do not retry every frame */
} s;

static void destroy_frozen(void) {
  if (s.acquired) vkDestroySemaphore(s.device, s.acquired, NULL);
  if (s.sampler) vkDestroySampler(s.device, s.sampler, NULL);
  if (s.frozen_view) vkDestroyImageView(s.device, s.frozen_view, NULL);
  if (s.frozen) vkDestroyImage(s.device, s.frozen, NULL);
  if (s.frozen_memory) vkFreeMemory(s.device, s.frozen_memory, NULL);
  s.acquired = VK_NULL_HANDLE;
  s.sampler = VK_NULL_HANDLE;
  s.frozen_view = VK_NULL_HANDLE;
  s.frozen = VK_NULL_HANDLE;
  s.frozen_memory = VK_NULL_HANDLE;
}

void tico_vk_overlay_reset(void) {
  if (!s.device) return;
  vkDeviceWaitIdle(s.device);
  destroy_frozen();
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

  uint32_t count = MAX_IMAGES;
  VkResult result = vkGetSwapchainImagesKHR(s.device, s.swapchain, &count, s.images);
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
      .image = s.images[i],
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
  tico_log("overlay: Vulkan present path ready (%u images, %ux%u, format %d%s)\n", count,
           info->extent.width, info->extent.height, (int)info->format,
           info->copyable ? "" : ", cannot hold");
  return 1;
}

/* The frozen frame's image, made the first time the menu holds. */
static int setup_frozen(void) {
  if (s.frozen) return 1;
  const VkImageCreateInfo image_info = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D,
    .format = s.info.format,
    .extent = {s.info.extent.width, s.info.extent.height, 1},
    .mipLevels = 1,
    .arrayLayers = 1,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  if (vkCreateImage(s.device, &image_info, NULL, &s.frozen) != VK_SUCCESS) return 0;
  VkMemoryRequirements requirements;
  vkGetImageMemoryRequirements(s.device, s.frozen, &requirements);
  VkPhysicalDeviceMemoryProperties memory;
  vkGetPhysicalDeviceMemoryProperties(s.info.physical_device, &memory);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; i++)
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      type = i;
  if (type == UINT32_MAX) return 0;
  const VkMemoryAllocateInfo allocate = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = requirements.size,
    .memoryTypeIndex = type,
  };
  if (vkAllocateMemory(s.device, &allocate, NULL, &s.frozen_memory) != VK_SUCCESS ||
      vkBindImageMemory(s.device, s.frozen, s.frozen_memory, 0) != VK_SUCCESS)
    return 0;
  const VkImageViewCreateInfo view_info = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .image = s.frozen,
    .viewType = VK_IMAGE_VIEW_TYPE_2D,
    .format = s.info.format,
    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  const VkSamplerCreateInfo sampler_info = {
    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
    .magFilter = VK_FILTER_LINEAR,
    .minFilter = VK_FILTER_LINEAR,
    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    .maxLod = 1.0f,
  };
  const VkSemaphoreCreateInfo semaphore_info = {
    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
  };
  return vkCreateImageView(s.device, &view_info, NULL, &s.frozen_view) == VK_SUCCESS &&
         vkCreateSampler(s.device, &sampler_info, NULL, &s.sampler) == VK_SUCCESS &&
         vkCreateSemaphore(s.device, &semaphore_info, NULL, &s.acquired) == VK_SUCCESS;
}

static int ready_for(const TicoVkSwapchainInfo *swapchain) {
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
  return !s.failed;
}

static void barrier(VkCommandBuffer command, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_access, VkAccessFlags dst_access,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
  const VkImageMemoryBarrier image_barrier = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .srcAccessMask = src_access,
    .dstAccessMask = dst_access,
    .oldLayout = from,
    .newLayout = to,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = image,
    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
  };
  vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &image_barrier);
}

/* Starts image index's command buffer, once the GPU is done with it. */
static VkCommandBuffer begin_commands(uint32_t index) {
  vkWaitForFences(s.device, 1, &s.fences[index], VK_TRUE, UINT64_MAX);
  vkResetFences(s.device, 1, &s.fences[index]);
  VkCommandBuffer command = s.commands[index];
  vkResetCommandBuffer(command, 0);
  const VkCommandBufferBeginInfo begin = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  vkBeginCommandBuffer(command, &begin);
  return command;
}

/* The overlay's render pass over image index. */
static void record_overlay(VkQueue queue, VkCommandBuffer command, uint32_t index) {
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
}

/* Submits image index's commands after the waits, signalling done[index]. */
static int submit(VkQueue queue, uint32_t index, uint32_t wait_count, const VkSemaphore *waits) {
  VkPipelineStageFlags stages[MAX_IMAGES];
  if (wait_count > MAX_IMAGES) wait_count = MAX_IMAGES;
  for (uint32_t i = 0; i < wait_count; i++)
    stages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                VK_PIPELINE_STAGE_TRANSFER_BIT;
  const VkSubmitInfo info = {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .waitSemaphoreCount = wait_count,
    .pWaitSemaphores = waits,
    .pWaitDstStageMask = stages,
    .commandBufferCount = 1,
    .pCommandBuffers = &s.commands[index],
    .signalSemaphoreCount = 1,
    .pSignalSemaphores = &s.done[index],
  };
  if (vkQueueSubmit(queue, 1, &info, s.fences[index]) == VK_SUCCESS) return 1;
  /* the fence stays unsignalled: make a fresh one so the next wait returns */
  vkDestroyFence(s.device, s.fences[index], NULL);
  const VkFenceCreateInfo fence_info = {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
  };
  vkCreateFence(s.device, &fence_info, NULL, &s.fences[index]);
  return 0;
}

int tico_vk_overlay_present(VkQueue queue, const VkPresentInfoKHR *present,
                            const TicoVkSwapchainInfo *swapchain,
                            VkPresentInfoKHR *out) {
  if (!tico_vk_overlay_hook || !tico_overlay_wants_frame || !tico_overlay_wants_frame())
    return 0;
  if (!present || present->swapchainCount != 1 || !present->pImageIndices ||
      present->pSwapchains[0] != swapchain->swapchain || !ready_for(swapchain))
    return 0;
  const uint32_t index = present->pImageIndices[0];
  if (index >= s.image_count) return 0;
  VkCommandBuffer command = begin_commands(index);
  record_overlay(queue, command, index);
  vkEndCommandBuffer(command);
  if (!submit(queue, index, present->waitSemaphoreCount, present->pWaitSemaphores)) return 0;
  *out = *present;
  out->waitSemaphoreCount = 1;
  out->pWaitSemaphores = &s.done[index];
  return 1;
}

int tico_vk_overlay_hold(VkQueue queue, const VkPresentInfoKHR *present,
                         const TicoVkSwapchainInfo *swapchain, TicoVkPresentFn present_fn,
                         VkResult *result) {
  if (!tico_vk_overlay_hook || !present || present->swapchainCount != 1 ||
      !present->pImageIndices || present->pSwapchains[0] != swapchain->swapchain ||
      !swapchain->copyable || !ready_for(swapchain))
    return 0;
  if (!setup_frozen()) {
    tico_log("overlay: could not make the frozen frame; the menu cannot hold\n");
    destroy_frozen();
    s.info.copyable = 0;
    return 0;
  }
  uint32_t index = present->pImageIndices[0];
  if (index >= s.image_count) return 0;

  /* the core's frame: kept as the background, and presented with the menu */
  VkCommandBuffer command = begin_commands(index);
  barrier(command, s.images[index], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
  barrier(command, s.frozen, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
  const VkImageCopy copy = {
    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
    .extent = {s.info.extent.width, s.info.extent.height, 1},
  };
  vkCmdCopyImage(command, s.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.frozen,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  barrier(command, s.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  barrier(command, s.frozen, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
  record_overlay(queue, command, index);
  vkEndCommandBuffer(command);
  if (!submit(queue, index, present->waitSemaphoreCount, present->pWaitSemaphores)) return 0;
  VkPresentInfoKHR first = *present;
  first.waitSemaphoreCount = 1;
  first.pWaitSemaphores = &s.done[index];
  *result = present_fn(queue, &first);

  const unsigned long long background = tico_overlay_vk_add_texture(s.sampler, s.frozen_view);
  tico_overlay_set_background(background, 0.0f, 0.0f, 1.0f, 1.0f);
  int frames = 0;
  while (background && __atomic_load_n(&tico_overlay_hold, __ATOMIC_ACQUIRE)) {
    /* the acquire semaphore is free again once the last frame's work is done */
    vkWaitForFences(s.device, 1, &s.fences[index], VK_TRUE, UINT64_MAX);
    uint32_t next = 0;
    const VkResult acquired = vkAcquireNextImageKHR(s.device, s.swapchain, UINT64_MAX,
                                                    s.acquired, VK_NULL_HANDLE, &next);
    if ((acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) || next >= s.image_count) {
      tico_log("overlay: acquire while holding failed (%d)\n", (int)acquired);
      break;
    }
    index = next;
    command = begin_commands(index);
    record_overlay(queue, command, index);
    vkEndCommandBuffer(command);
    if (!submit(queue, index, 1, &s.acquired)) break;
    const VkPresentInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &s.done[index],
      .swapchainCount = 1,
      .pSwapchains = &s.swapchain,
      .pImageIndices = &index,
    };
    const VkResult presented = present_fn(queue, &info);
    frames++;
    if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR) {
      tico_log("overlay: present while holding failed (%d)\n", (int)presented);
      break;
    }
  }
  vkQueueWaitIdle(queue);
  tico_overlay_set_background(0, 0.0f, 0.0f, 1.0f, 1.0f);
  tico_overlay_vk_remove_texture(background);
  tico_log("overlay: held the core for %d frames\n", frames);
  return 1;
}
