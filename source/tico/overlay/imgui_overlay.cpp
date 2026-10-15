// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include <switch.h>

#include <GLES2/gl2.h>
#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_vulkan.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define NANOSVG_IMPLEMENTATION
#include <nanosvg.h>
#define NANOSVGRAST_IMPLEMENTATION
#include <nanosvgrast.h>

#include "../TicoSession.h"
#include "tico/overlay_hook.h"
extern "C" {
#include "tico/tico_entry.h"
}

#include "tico/overlay/imgui_overlay.h"
#include "tico/overlay/tico_config.h"

namespace SwitchFrontend::ImGuiOverlay {
namespace {

constexpr const char* TAG = "[tico-overlay]";
constexpr std::array<const char*, 3> kFontPaths = {{
    "romfs:/fonts/font.ttf",
    "sdmc:/tico/fonts/font.ttf",
    "sdmc:/tico/system/nethersx2/fonts/font.ttf",
}};
constexpr std::array<const char*, 6> kAvatarPaths = {{
    "sdmc:/tico/assets/avatar.jpg",
    "sdmc:/tico/assets/avatar.jpeg",
    "sdmc:/tico/assets/avatar.png",
    "romfs:/assets/avatar.jpg",
    "romfs:/assets/avatar.jpeg",
    "romfs:/assets/avatar.png",
}};
// Selection border strips, as tico-nx ships them: index 0 and "original" (the
// last) use the cyan/violet strip, every other tint has its own. The slugs
// mirror tico-nx's TintPalette::GetBorderGradientFile.
constexpr const char* kBorderDir = "romfs:/assets/border/";
constexpr std::array<const char*, 13> kBorderSlugs = {{
    "default", "aqua", "violet", "sunset", "lime", "rose", "gold", "ice", "ember", "mint",
    "lagoon", "cobalt", "original",
}};
// the overlay's layout is designed for a 720p display
constexpr float kDesignHeight = 720.0f;

// The core presents on its own thread while the host feeds input on the main
// one: everything the overlay keeps is used under this lock.
std::recursive_mutex s_mutex;
bool s_initialized = false;
// The frame the game was showing when the menu froze it, drawn under the
// menu while the presenting thread holds (tico/overlay_hook.h); 0 for none.
unsigned long long s_background = 0;
ImVec2 s_background_uv0{0.0f, 0.0f};
ImVec2 s_background_uv1{1.0f, 1.0f};
bool s_vulkan = false;
bool s_visible = false;
bool s_psm_initialized = false;
OverlayUI::NavInput s_nav{};
OverlayUI::Action s_action = OverlayUI::Action::None;

// An image decoded once and uploaded when a backend is ready.
struct DecodedImage {
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
};
DecodedImage s_avatar;
DecodedImage s_border;
// the quick menu sidebar's icons: Settings, Restart, Exit Game
std::array<DecodedImage, 3> s_icons;
bool s_avatar_decoded = false;

// Textures made while drawing (the Save/Load State pictures): destroyed at
// the start of the next drawn frame, once the GPU is done with them.
std::vector<unsigned long long> s_retired_textures;

bool DecodeImage(DecodedImage& image, unsigned char* rgba, int width, int height,
                 const char* source) {
    if (!rgba) {
        return false;
    }
    image.rgba.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
    image.width = width;
    image.height = height;
    stbi_image_free(rgba);
    tico_log("%s loaded %s (%dx%d)\n", TAG, source, width, height);
    return true;
}

bool DecodeAvatar(unsigned char* rgba, int width, int height, const char* source) {
    return DecodeImage(s_avatar, rgba, width, height, source);
}

void DecodeBorder() {
    const int tint = TicoConfig::BorderTint();
    const int original = static_cast<int>(kBorderSlugs.size()) - 1;
    std::string path = kBorderDir;
    if (tint <= 0 || tint >= original) {
        path += "border_gradient.png";
    } else {
        path += std::string("border_gradient_") + kBorderSlugs[static_cast<std::size_t>(tint)] +
                ".png";
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    if (!DecodeImage(s_border, stbi_load(path.c_str(), &width, &height, &channels, 4), width,
                     height, path.c_str())) {
        tico_log("%s no selection border strip at %s\n", TAG, path.c_str());
    }
}

bool DecodeAvatarFromAccount() {
    // The System account service, not Application: opening Application registers
    // the process with ns each time, and in tico's chain of NROs in one process
    // (tico, a game, tico, a game) the fourth registration makes ns abort.
    if (R_FAILED((R_SUCCEEDED(accountInitialize(AccountServiceType_System)) ? 0 : accountInitialize(AccountServiceType_Application)))) {
        return false;
    }

    AccountUid uid{};
    bool found = tico::CurrentSession().AccountId(uid); // Who tico says plays
    if (!found) { // A module gets no session: the account tico's saves folder names
        unsigned long long hi = 0, lo = 0;
        const char* id = tico_account_id();
        found = std::strlen(id) == 32 && std::sscanf(id, "%16llx%16llx", &hi, &lo) == 2;
        uid.uid[0] = hi;
        uid.uid[1] = lo;
        found = found && accountUidIsValid(&uid);
    }
    if (!found) found = R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid);
    if (!found) {
        found = R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid);
    }
    if (!found) {
        AccountUid uids[ACC_USER_LIST_SIZE]{};
        s32 total = 0;
        if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &total)) && total > 0) {
            uid = uids[0];
            found = accountUidIsValid(&uid);
        }
    }

    bool decoded = false;
    AccountProfile profile{};
    if (found && R_SUCCEEDED(accountGetProfile(&profile, uid))) {
        AccountProfileBase profile_base{};
        if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &profile_base)) &&
            profile_base.nickname[0] != '\0') {
            OverlayUI::SetNickname(profile_base.nickname);
        }
        u32 image_size = 0;
        if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &image_size)) && image_size > 0) {
            std::vector<unsigned char> jpeg(image_size);
            u32 actual = 0;
            if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpeg.data(), image_size, &actual)) &&
                actual > 0) {
                int width = 0;
                int height = 0;
                int channels = 0;
                decoded = DecodeAvatar(stbi_load_from_memory(jpeg.data(), static_cast<int>(actual),
                                                             &width, &height, &channels, 4),
                                       width, height, "switch-account-avatar");
            }
        }
        accountProfileClose(&profile);
    }
    accountExit();
    return decoded;
}

void DecodeAvatarOnce() {
    if (s_avatar_decoded) {
        return;
    }
    s_avatar_decoded = true;
    for (const char* path : tico::AvatarCandidates(kAvatarPaths)) { // Only the user's own
        int width = 0;
        int height = 0;
        int channels = 0;
        if (DecodeAvatar(stbi_load(path, &width, &height, &channels, 4), width, height, path)) {
            return;
        }
    }
    if (!DecodeAvatarFromAccount()) {
        tico_log("%s no avatar image found\n", TAG);
    }
}

// A white SVG icon rasterized to a size x size image.
void DecodeSvgIcon(DecodedImage& image, const std::string& path, int size) {
    NSVGimage* svg = nsvgParseFromFile(path.c_str(), "px", 96.0f);
    if (!svg) {
        tico_log("%s no icon at %s\n", TAG, path.c_str());
        return;
    }
    if (NSVGrasterizer* rast = nsvgCreateRasterizer()) {
        image.rgba.assign(static_cast<std::size_t>(size) * size * 4, 0);
        const float longest = std::max(svg->width, svg->height);
        const float scale = longest > 0.0f ? size / longest : 1.0f;
        // centre the shorter side
        const float dx = (size - (svg->width * scale)) * 0.5f;
        const float dy = (size - (svg->height * scale)) * 0.5f;
        nsvgRasterize(rast, svg, dx, dy, scale, image.rgba.data(), size, size, size * 4);
        nsvgDeleteRasterizer(rast);
        image.width = size;
        image.height = size;
    }
    nsvgDelete(svg);
}

void DecodeSidebarIcons() {
    const char* names[] = {"gear.svg", "rotate-left.svg", "right-from-bracket.svg"};
    for (std::size_t i = 0; i < s_icons.size(); ++i) {
        DecodeSvgIcon(s_icons[i], std::string("romfs:/assets/icons/") + names[i], 64);
    }
}

void BeginFrame(float width, float height) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = 1.0f / 60.0f;
    io.FontGlobalScale = height / kDesignHeight;
    OverlayUI::FeedNav(s_nav);
    s_nav = {};
}

// Draws the menu or the HUD for this frame. Returns false when there is
// nothing to draw, so the renderer is left alone.
bool BuildFrame(float width, float height) {
    if (!s_visible && !OverlayUI::HasTransientContent() && !s_background) {
        return false;
    }
    BeginFrame(width, height);
    ImGui::NewFrame();
    if (s_background) {
        ImGui::GetBackgroundDrawList()->AddImage(
            ImTextureRef(static_cast<ImTextureID>(s_background)), ImVec2(0.0f, 0.0f),
            ImVec2(width, height), s_background_uv0, s_background_uv1);
    }
    const OverlayUI::Action action =
        OverlayUI::Render(static_cast<int>(width), static_cast<int>(height));
    ImGui::Render();
    if (action != OverlayUI::Action::None) {
        s_action = action;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Vulkan (NVK)

struct VulkanState {
    bool ready = false;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
};

struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
};
VulkanState s_vk;
VulkanTexture s_vk_avatar;
VulkanTexture s_vk_border;
std::array<VulkanTexture, 3> s_vk_icons;
// textures made with CreateTexture, by ImGui id
std::map<unsigned long long, VulkanTexture> s_vk_dynamic;

bool FindMemoryType(u32 type_filter, VkMemoryPropertyFlags properties, u32& out_index) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(s_vk.physical_device, &memory);
    for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            out_index = i;
            return true;
        }
    }
    return false;
}

void TransitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout old_layout,
                     VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
                     VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void DestroyVulkanTexture(VulkanTexture& texture) {
    if (texture.descriptor)
        ImGui_ImplVulkan_RemoveTexture(texture.descriptor);
    if (texture.sampler)
        vkDestroySampler(s_vk.device, texture.sampler, nullptr);
    if (texture.view)
        vkDestroyImageView(s_vk.device, texture.view, nullptr);
    if (texture.image)
        vkDestroyImage(s_vk.device, texture.image, nullptr);
    if (texture.memory)
        vkFreeMemory(s_vk.device, texture.memory, nullptr);
    texture = {};
}

// Uploads RGBA pixels with a one-off submission. Runs before the renderer
// submits the frame being recorded, so the queue is free. Returns the ImGui
// texture id, or 0 when the upload failed.
unsigned long long UploadVulkanPixels(const unsigned char* rgba, int source_width,
                                      int source_height, VulkanTexture& texture) {
    if (!rgba || source_width <= 0 || source_height <= 0 || texture.descriptor) {
        return 0;
    }
    const u32 width = static_cast<u32>(source_width);
    const u32 height = static_cast<u32>(source_height);
    const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;

    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    auto finish = [&](const char* failure) {
        if (command)
            vkFreeCommandBuffers(s_vk.device, s_vk.command_pool, 1, &command);
        if (staging)
            vkDestroyBuffer(s_vk.device, staging, nullptr);
        if (staging_memory)
            vkFreeMemory(s_vk.device, staging_memory, nullptr);
        if (failure) {
            tico_log("%s texture upload failed: %s\n", TAG, failure);
            DestroyVulkanTexture(texture);
        }
        return 0ull;
    };

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(s_vk.device, &buffer_info, nullptr, &staging) != VK_SUCCESS)
        return finish("staging buffer");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(s_vk.device, staging, &requirements);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = requirements.size;
    if (!FindMemoryType(requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        alloc.memoryTypeIndex))
        return finish("no host-visible memory");
    void* mapped = nullptr;
    if (vkAllocateMemory(s_vk.device, &alloc, nullptr, &staging_memory) != VK_SUCCESS ||
        vkBindBufferMemory(s_vk.device, staging, staging_memory, 0) != VK_SUCCESS ||
        vkMapMemory(s_vk.device, staging_memory, 0, size, 0, &mapped) != VK_SUCCESS)
        return finish("staging memory");
    std::memcpy(mapped, rgba, static_cast<std::size_t>(size));
    vkUnmapMemory(s_vk.device, staging_memory);

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_vk.device, &image_info, nullptr, &texture.image) != VK_SUCCESS)
        return finish("image");
    vkGetImageMemoryRequirements(s_vk.device, texture.image, &requirements);
    alloc.allocationSize = requirements.size;
    if (!FindMemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        alloc.memoryTypeIndex))
        return finish("no device-local memory");
    if (vkAllocateMemory(s_vk.device, &alloc, nullptr, &texture.memory) != VK_SUCCESS ||
        vkBindImageMemory(s_vk.device, texture.image, texture.memory, 0) != VK_SUCCESS)
        return finish("image memory");

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = texture.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(s_vk.device, &view_info, nullptr, &texture.view) != VK_SUCCESS)
        return finish("image view");

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(s_vk.device, &sampler_info, nullptr, &texture.sampler) != VK_SUCCESS)
        return finish("sampler");

    VkCommandBufferAllocateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_info.commandPool = s_vk.command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(s_vk.device, &command_info, &command) != VK_SUCCESS)
        return finish("command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command, &begin);
    TransitionImage(command, texture.image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(command, staging, texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TransitionImage(command, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkEndCommandBuffer(command);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    if (vkQueueSubmit(s_vk.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
        return finish("queue submit");
    vkQueueWaitIdle(s_vk.queue);

    texture.descriptor = ImGui_ImplVulkan_AddTexture(
        texture.sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    finish(nullptr);
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(texture.descriptor));
}

// A decoded image, uploaded once; the pixels are dropped either way so a
// failed upload is not retried every frame.
unsigned long long UploadVulkanTexture(DecodedImage& source, VulkanTexture& texture) {
    if (source.rgba.empty() || texture.descriptor) {
        return 0;
    }
    const unsigned long long id =
        UploadVulkanPixels(source.rgba.data(), source.width, source.height, texture);
    source.rgba.clear();
    return id;
}

unsigned long long VulkanId(const VulkanTexture& texture) {
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(texture.descriptor));
}

void UploadVulkanTextures() {
    if (const unsigned long long id = UploadVulkanTexture(s_avatar, s_vk_avatar)) {
        OverlayUI::SetAvatarTextureId(id);
    }
    if (const unsigned long long id = UploadVulkanTexture(s_border, s_vk_border)) {
        OverlayUI::SetBorderTextureId(id);
    }
    bool icons = false;
    for (std::size_t i = 0; i < s_icons.size(); ++i) {
        icons |= UploadVulkanTexture(s_icons[i], s_vk_icons[i]) != 0;
    }
    if (icons) {
        OverlayUI::SetSidebarIconTextures(VulkanId(s_vk_icons[0]), VulkanId(s_vk_icons[1]),
                                          VulkanId(s_vk_icons[2]));
    }
}

void DestroyRetiredVulkanTextures() {
    if (s_retired_textures.empty()) {
        return;
    }
    // the frames that drew them may still be in flight
    vkQueueWaitIdle(s_vk.queue);
    for (const unsigned long long id : s_retired_textures) {
        const auto it = s_vk_dynamic.find(id);
        if (it != s_vk_dynamic.end()) {
            DestroyVulkanTexture(it->second);
            s_vk_dynamic.erase(it);
        }
    }
    s_retired_textures.clear();
}

void DestroyVulkanTextures() {
    DestroyVulkanTexture(s_vk_avatar);
    DestroyVulkanTexture(s_vk_border);
    for (VulkanTexture& icon : s_vk_icons) {
        DestroyVulkanTexture(icon);
    }
    for (auto& [id, texture] : s_vk_dynamic) {
        DestroyVulkanTexture(texture);
    }
    s_vk_dynamic.clear();
    s_retired_textures.clear();
    OverlayUI::SetAvatarTextureId(0);
    OverlayUI::SetBorderTextureId(0);
    OverlayUI::SetSidebarIconTextures(0, 0, 0);
}

void DestroyVulkanBackend() {
    if (s_vk.device) {
        vkDeviceWaitIdle(s_vk.device);
    }
    if (s_vk.ready) {
        DestroyVulkanTextures();
        ImGui_ImplVulkan_Shutdown();
    }
    if (s_vk.descriptor_pool)
        vkDestroyDescriptorPool(s_vk.device, s_vk.descriptor_pool, nullptr);
    if (s_vk.command_pool)
        vkDestroyCommandPool(s_vk.device, s_vk.command_pool, nullptr);
    s_vk = {};
}

// The backend draws inside the renderer's final render pass, so it is built
// against that pass and rebuilt if the renderer ever hands over another one.
bool EnsureVulkanBackend(const TicoVkOverlayContext& context) {
    if (s_vk.ready && s_vk.render_pass == context.render_pass && s_vk.device == context.device) {
        return true;
    }
    DestroyVulkanBackend();
    s_vk.physical_device = context.physical_device;
    s_vk.device = context.device;
    s_vk.queue = context.queue;
    s_vk.render_pass = context.render_pass;

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64};
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 64;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkCommandPoolCreateInfo command_info{};
    command_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    command_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                         VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_info.queueFamilyIndex = context.queue_family;
    if (vkCreateDescriptorPool(s_vk.device, &pool_info, nullptr, &s_vk.descriptor_pool) !=
            VK_SUCCESS ||
        vkCreateCommandPool(s_vk.device, &command_info, nullptr, &s_vk.command_pool) !=
            VK_SUCCESS) {
        tico_log("%s could not create the Vulkan pools\n", TAG);
        DestroyVulkanBackend();
        return false;
    }

    const u32 image_count = context.image_count >= 2 ? context.image_count : 2;
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_1;
    init.Instance = context.instance;
    init.PhysicalDevice = context.physical_device;
    init.Device = context.device;
    init.QueueFamily = context.queue_family;
    init.Queue = context.queue;
    init.DescriptorPool = s_vk.descriptor_pool;
    init.MinImageCount = image_count;
    init.ImageCount = image_count;
    init.PipelineInfoMain.RenderPass = context.render_pass;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.PipelineInfoMain.Subpass = 0;
    if (!ImGui_ImplVulkan_Init(&init)) {
        tico_log("%s ImGui_ImplVulkan_Init failed\n", TAG);
        DestroyVulkanBackend();
        return false;
    }
    s_vk.ready = true;
    tico_log("%s Vulkan backend ready (format=%d, images=%u)\n", TAG,
             static_cast<int>(context.format), image_count);
    return true;
}

void VulkanHook(const TicoVkOverlayContext* context) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || !context) {
        return;
    }
    if (!s_visible && !OverlayUI::HasTransientContent() && !s_background) {
        return;
    }
    if (!EnsureVulkanBackend(*context)) {
        return;
    }
    DestroyRetiredVulkanTextures();
    UploadVulkanTextures();
    ImGui_ImplVulkan_NewFrame();
    if (BuildFrame(static_cast<float>(context->width), static_cast<float>(context->height))) {
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), context->command_buffer);
    }
}

// ---------------------------------------------------------------------------
// OpenGL ES (NVC0 and Zink)

bool s_gl_ready = false;
GLuint s_gl_avatar = 0;
GLuint s_gl_border = 0;
std::array<GLuint, 3> s_gl_icons = {};
std::vector<GLuint> s_gl_dynamic;

GLuint UploadGlPixels(const unsigned char* rgba, int width, int height) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return texture;
}

// Returns the texture's ImGui id, or 0 when there was nothing to upload.
unsigned long long UploadGlTexture(DecodedImage& source, GLuint& texture) {
    if (source.rgba.empty() || texture) {
        return 0;
    }
    texture = UploadGlPixels(source.rgba.data(), source.width, source.height);
    source.rgba.clear();
    return static_cast<unsigned long long>(texture);
}

void UploadGlTextures() {
    if (const unsigned long long id = UploadGlTexture(s_avatar, s_gl_avatar)) {
        OverlayUI::SetAvatarTextureId(id);
    }
    if (const unsigned long long id = UploadGlTexture(s_border, s_gl_border)) {
        OverlayUI::SetBorderTextureId(id);
    }
    bool icons = false;
    for (std::size_t i = 0; i < s_icons.size(); ++i) {
        icons |= UploadGlTexture(s_icons[i], s_gl_icons[i]) != 0;
    }
    if (icons) {
        OverlayUI::SetSidebarIconTextures(s_gl_icons[0], s_gl_icons[1], s_gl_icons[2]);
    }
}

void DestroyRetiredGlTextures() {
    for (const unsigned long long id : s_retired_textures) {
        GLuint texture = static_cast<GLuint>(id);
        const auto it = std::find(s_gl_dynamic.begin(), s_gl_dynamic.end(), texture);
        if (it != s_gl_dynamic.end()) {
            glDeleteTextures(1, &texture);
            s_gl_dynamic.erase(it);
        }
    }
    s_retired_textures.clear();
}

void GlHook(int width, int height) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized) {
        return;
    }
    if (!s_visible && !OverlayUI::HasTransientContent() && !s_background) {
        return;
    }
    if (!s_gl_ready) {
        // the core creates an OpenGL ES 3 context
        if (!ImGui_ImplOpenGL3_Init("#version 300 es")) {
            tico_log("%s ImGui_ImplOpenGL3_Init failed\n", TAG);
            return;
        }
        s_gl_ready = true;
        tico_log("%s OpenGL backend ready\n", TAG);
    }
    DestroyRetiredGlTextures();
    UploadGlTextures();
    ImGui_ImplOpenGL3_NewFrame();
    if (BuildFrame(static_cast<float>(width), static_cast<float>(height))) {
        glViewport(0, 0, width, height);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
}

void DestroyGlBackend() {
    for (GLuint* texture : {&s_gl_avatar, &s_gl_border, &s_gl_icons[0], &s_gl_icons[1],
                            &s_gl_icons[2]}) {
        if (*texture) {
            glDeleteTextures(1, texture);
            *texture = 0;
        }
    }
    for (GLuint texture : s_gl_dynamic) {
        glDeleteTextures(1, &texture);
    }
    s_gl_dynamic.clear();
    s_retired_textures.clear();
    OverlayUI::SetAvatarTextureId(0);
    OverlayUI::SetBorderTextureId(0);
    OverlayUI::SetSidebarIconTextures(0, 0, 0);
    if (s_gl_ready) {
        ImGui_ImplOpenGL3_Shutdown();
        s_gl_ready = false;
    }
}

int WantsFrame() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_initialized && (s_visible || OverlayUI::HasTransientContent() || s_background);
}

} // namespace

bool Init(bool vulkan) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (s_initialized) {
        return true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    for (const char* font_path : kFontPaths) {
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(font_path, 32.0f)) {
            io.FontDefault = font;
            tico_log("%s loaded font: %s\n", TAG, font_path);
            break;
        }
    }
    if (!io.FontDefault) {
        tico_log("%s could not load overlay font, using ImGui default\n", TAG);
    }
    ImGui::StyleColorsDark();

    if (!s_psm_initialized && R_SUCCEEDED(psmInitialize())) {
        s_psm_initialized = true;
    }
    DecodeAvatarOnce();
    DecodeBorder();
    DecodeSidebarIcons();

    s_vulkan = vulkan;
    s_visible = false;
    s_action = OverlayUI::Action::None;
    s_initialized = true;
    if (vulkan) {
        tico_vk_overlay_hook = &VulkanHook;
    } else {
        tico_gl_overlay_hook = &GlHook;
    }
    tico_overlay_wants_frame = &WantsFrame;
    tico_log("%s initialized for %s\n", TAG, vulkan ? "Vulkan" : "OpenGL");
    return true;
}

void Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized) {
        return;
    }
    tico_vk_overlay_hook = nullptr;
    tico_gl_overlay_hook = nullptr;
    tico_overlay_wants_frame = nullptr;
    if (s_vulkan) {
        DestroyVulkanBackend();
    } else {
        DestroyGlBackend();
    }
    ImGui::DestroyContext();
    OverlayUI::SetVisible(false);
    OverlayUI::ShowToast(std::string{});
    if (s_psm_initialized) {
        psmExit();
        s_psm_initialized = false;
    }
    s_initialized = false;
}

void SetVisible(bool visible) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_visible = visible;
    s_nav = {};
    OverlayUI::SetVisible(visible);
}

bool IsVisible() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    return s_visible;
}

void FeedNav(const OverlayUI::NavInput& nav) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    s_nav.up |= nav.up;
    s_nav.down |= nav.down;
    s_nav.left |= nav.left;
    s_nav.right |= nav.right;
    s_nav.accept |= nav.accept;
    s_nav.cancel |= nav.cancel;
}

void FeedTouch(const OverlayUI::TouchInput& touch) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    OverlayUI::FeedTouch(touch);
}

unsigned long long CreateTexture(const unsigned char* rgba, int width, int height) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (!s_initialized || !rgba || width <= 0 || height <= 0) {
        return 0;
    }
    if (s_vulkan) {
        if (!s_vk.ready) {
            return 0;
        }
        VulkanTexture texture;
        const unsigned long long id = UploadVulkanPixels(rgba, width, height, texture);
        if (id) {
            s_vk_dynamic[id] = texture;
        }
        return id;
    }
    if (!s_gl_ready) {
        return 0;
    }
    const GLuint texture = UploadGlPixels(rgba, width, height);
    s_gl_dynamic.push_back(texture);
    return texture;
}

void DestroyTexture(unsigned long long texture) {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    if (texture) {
        s_retired_textures.push_back(texture);
    }
}

std::recursive_mutex& Mutex() {
    return s_mutex;
}

OverlayUI::Action ConsumeAction() {
    std::lock_guard<std::recursive_mutex> lock(s_mutex);
    const OverlayUI::Action action = s_action;
    s_action = OverlayUI::Action::None;
    return action;
}

} // namespace SwitchFrontend::ImGuiOverlay

// The frozen frame under the menu (tico/overlay_hook.h), for the renderers'
// present hooks.
extern "C" void tico_overlay_set_background(unsigned long long texture, float u0, float v0,
                                            float u1, float v1) {
    std::lock_guard<std::recursive_mutex> lock(SwitchFrontend::ImGuiOverlay::s_mutex);
    SwitchFrontend::ImGuiOverlay::s_background = texture;
    SwitchFrontend::ImGuiOverlay::s_background_uv0 = ImVec2(u0, v0);
    SwitchFrontend::ImGuiOverlay::s_background_uv1 = ImVec2(u1, v1);
}

extern "C" unsigned long long tico_overlay_vk_add_texture(VkSampler sampler, VkImageView view) {
    std::lock_guard<std::recursive_mutex> lock(SwitchFrontend::ImGuiOverlay::s_mutex);
    if (!SwitchFrontend::ImGuiOverlay::s_vk.ready) {
        return 0;
    }
    return reinterpret_cast<unsigned long long>(
        ImGui_ImplVulkan_AddTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
}

extern "C" void tico_overlay_vk_remove_texture(unsigned long long texture) {
    std::lock_guard<std::recursive_mutex> lock(SwitchFrontend::ImGuiOverlay::s_mutex);
    if (texture && SwitchFrontend::ImGuiOverlay::s_vk.ready) {
        ImGui_ImplVulkan_RemoveTexture(reinterpret_cast<VkDescriptorSet>(texture));
    }
}
