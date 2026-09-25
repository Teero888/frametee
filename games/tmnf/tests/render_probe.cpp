// Run the TMNF module on a Vulkan device without a window or editor state.
// Usage: tmnf_render_probe module.so data/games/tmnf map.Gbx output.ppm
//        [start|chase|checkpoint|water|x,y,z,tx,ty,tz] [tick] [exercise|bench] [reload-map.Gbx]
#include "tmnf_internal.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <stdexcept>
#include <vulkan/vulkan.h>

namespace {
VkInstance instance;
VkPhysicalDevice physical;
VkDevice device;
VkQueue queue;
VkCommandPool pool;
uint32_t family;
std::string data_root, output;
ft_camera camera{};
ft_gpu_device gpu{};
double queue_wait_ms = 0;
uint32_t captures = 0;
std::atomic<unsigned> validation_errors{0};
VkDebugUtilsMessengerEXT messenger{};
VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                          VkDebugUtilsMessageTypeFlagsEXT,
                                          const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
  if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validation_errors;
  std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
  return VK_FALSE;
}
struct Texture {
  VkImage image{};
  VkDeviceMemory memory{};
  uint32_t width{}, height{};
  bool fresh = true;
};

void Check(VkResult result) {
  if (result != VK_SUCCESS) throw std::runtime_error("Vulkan error " + std::to_string(result));
}
uint32_t Memory(uint32_t bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties p;
  vkGetPhysicalDeviceMemoryProperties(physical, &p);
  for (uint32_t i = 0; i < p.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags) return i;
  throw std::runtime_error("Vulkan memory type unavailable");
}
void Commands(const std::function<void(VkCommandBuffer)> &record) {
  VkCommandBufferAllocateInfo a{};
  a.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  a.commandPool = pool;
  a.commandBufferCount = 1;
  VkCommandBuffer cmd;
  Check(vkAllocateCommandBuffers(device, &a, &cmd));
  VkCommandBufferBeginInfo b{};
  b.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  Check(vkBeginCommandBuffer(cmd, &b));
  record(cmd);
  Check(vkEndCommandBuffer(cmd));
  VkSubmitInfo s{};
  s.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  s.commandBufferCount = 1;
  s.pCommandBuffers = &cmd;
  Check(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE));
  Check(vkQueueWaitIdle(queue));
  vkFreeCommandBuffers(device, pool, 1, &cmd);
}
void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout before, VkImageLayout after) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout = before;
  b.newLayout = after;
  b.srcAccessMask = before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b);
}
ft_texture *CreateTexture(const ft_texture_desc *desc) {
  auto t = std::make_unique<Texture>();
  t->width = desc->width;
  t->height = desc->height;
  VkImageCreateInfo i{};
  i.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  i.imageType = VK_IMAGE_TYPE_2D;
  i.format = VK_FORMAT_R8G8B8A8_UNORM;
  i.extent = {desc->width, desc->height, 1};
  i.mipLevels = i.arrayLayers = 1;
  i.samples = VK_SAMPLE_COUNT_1_BIT;
  i.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  Check(vkCreateImage(device, &i, nullptr, &t->image));
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, t->image, &req);
  VkMemoryAllocateInfo a{};
  a.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  a.allocationSize = req.size;
  a.memoryTypeIndex = Memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  Check(vkAllocateMemory(device, &a, nullptr, &t->memory));
  Check(vkBindImageMemory(device, t->image, t->memory, 0));
  Commands(
      [&](auto cmd) { Barrier(cmd, t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); });
  return reinterpret_cast<ft_texture *>(t.release());
}
void DestroyTexture(ft_texture *handle) {
  auto *t = reinterpret_cast<Texture *>(handle);
  vkDeviceWaitIdle(device);
  vkDestroyImage(device, t->image, nullptr);
  vkFreeMemory(device, t->memory, nullptr);
  delete t;
}
void Capture(ft_pipeline *, float, ft_mesh *, ft_texture *const *textures, uint32_t, const void *, size_t) {
  ++captures;
  auto &t = *reinterpret_cast<Texture *>(textures[0]);
  if (output.empty()) {
    const auto begin = std::chrono::steady_clock::now();
    if (t.fresh)
      Commands([&](auto cmd) {
        Barrier(cmd, t.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      });
    else Check(vkQueueWaitIdle(queue));
    t.fresh = false;
    queue_wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    return;
  }
  const uint32_t width = uint32_t(camera.viewport.x), height = uint32_t(camera.viewport.y);
  VkBufferCreateInfo b{};
  b.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  b.size = size_t(width) * height * 4;
  b.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer;
  Check(vkCreateBuffer(device, &b, nullptr, &buffer));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device, buffer, &req);
  VkMemoryAllocateInfo a{};
  a.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  a.allocationSize = req.size;
  a.memoryTypeIndex =
      Memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory memory;
  Check(vkAllocateMemory(device, &a, nullptr, &memory));
  Check(vkBindBufferMemory(device, buffer, memory, 0));
  Commands([&](auto cmd) {
    Barrier(cmd, t.image, t.fresh ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    Barrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  });
  t.fresh = false;
  if (!output.empty()) {
    void *pixels;
    Check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &pixels));
    FILE *file = std::fopen(output.c_str(), "wb");
    if (!file) throw std::runtime_error("Cannot open capture");
    std::fprintf(file, "P6\n%u %u\n255\n", width, height);
    for (size_t i = 0; i < size_t(width) * height; ++i)
      std::fwrite(static_cast<unsigned char *>(pixels) + i * 4, 1, 3, file);
    std::fclose(file);
    vkUnmapMemory(device, memory);
  }
  vkDestroyBuffer(device, buffer, nullptr);
  vkFreeMemory(device, memory, nullptr);
}
void View(ft_vec3 eye, ft_vec3 target, uint32_t width = 1280, uint32_t height = 720) {
  using namespace tmnf;
  camera.struct_size = sizeof(camera);
  camera.eye = eye;
  camera.target = target;
  camera.viewport = {float(width), float(height)};
  camera.aspect = float(width) / height;
  camera.forward = Normalize(Sub(target, eye));
  camera.up = {0, 1, 0};
  camera.near_z = .1f;
  camera.fov_y = kPi / 3;
  const auto z = Scale(camera.forward, -1), x = Normalize(Cross(camera.up, z)), y = Cross(z, x);
  float view[16] = {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -Dot(x, eye), -Dot(y, eye), -Dot(z, eye), 1};
  const float f = 1 / std::tan(camera.fov_y * .5f);
  float projection[16] = {f / camera.aspect, 0, 0, 0, 0, -f, 0, 0, 0, 0, 0, -1, 0, 0, .1f, 0};
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) {
      camera.view_proj[c * 4 + r] = 0;
      for (int k = 0; k < 4; ++k)
        camera.view_proj[c * 4 + r] += projection[k * 4 + r] * view[c * 4 + k];
    }
}
} // namespace
int main(int argc, char **argv) {
  if (argc < 5) {
    std::fprintf(stderr, "Usage: %s module.so data-root map.Gbx output.ppm [water|x,y,z,tx,ty,tz] [tick]\n", argv[0]);
    return 2;
  }
  try {
    data_root = argv[2];
    VkInstanceCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    uint32_t extension_count = 0;
    Check(vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr));
    std::vector<VkExtensionProperties> extensions(extension_count);
    Check(vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, extensions.data()));
    const char *debug_extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    for (const auto &extension : extensions)
      if (std::strcmp(extension.extensionName, debug_extension) == 0) {
        ii.enabledExtensionCount = 1;
        ii.ppEnabledExtensionNames = &debug_extension;
      }
    Check(vkCreateInstance(&ii, nullptr, &instance));
    if (ii.enabledExtensionCount) {
      VkDebugUtilsMessengerCreateInfoEXT debug{};
      debug.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
      debug.messageSeverity =
          VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      debug.pfnUserCallback = Validation;
      auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
      if (create) Check(create(instance, &debug, nullptr, &messenger));
    }
    uint32_t count = 0;
    Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    if (!count) throw std::runtime_error("No Vulkan device");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    physical = devices[0];
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::fprintf(stderr, "GPU: %s\n", properties.deviceName);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      ++family;
    float priority = 1;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    Check(vkCreateDevice(physical, &di, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.queueFamilyIndex = family;
    Check(vkCreateCommandPool(device, &pi, nullptr, &pool));
    gpu = {sizeof(gpu), FT_GPU_API_VULKAN, instance, physical, device, queue, family, VK_API_VERSION_1_0};
    ft_engine_api api{};
    api.struct_size = sizeof(api);
    api.gpu_device = []() -> const ft_gpu_device * { return &gpu; };
    api.log = [](ft_log_level, const char *category, const char *text) {
      std::fprintf(stderr, "[%s] %s\n", category, text);
    };
    api.resolve_data_path = [](const char *relative, char *out, size_t size) {
      std::string path = data_root + "/" + relative;
      if (out && size) std::snprintf(out, size, "%s", path.c_str());
      return path.size();
    };
    api.texture_create = CreateTexture;
    api.texture_destroy = DestroyTexture;
    api.texture_update_layer = [](ft_texture *, uint32_t, const void *, uint32_t, uint32_t) { return true; };
    api.texture_gpu_image = [](ft_texture *handle, ft_gpu_image *out) {
      auto &t = *reinterpret_cast<Texture *>(handle);
      *out = {sizeof(*out),
              reinterpret_cast<void *>(t.image),
              VK_FORMAT_R8G8B8A8_UNORM,
              t.width,
              t.height,
              1,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      return true;
    };
    api.pipeline_create = [](const ft_pipeline_desc *) { return reinterpret_cast<ft_pipeline *>(1); };
    api.pipeline_destroy = [](ft_pipeline *) {};
    api.mesh_create = [](const void *, uint32_t, uint32_t, const uint32_t *, uint32_t) {
      return reinterpret_cast<ft_mesh *>(1);
    };
    api.mesh_destroy = [](ft_mesh *) {};
    api.draw_mesh = Capture;
    api.draw_triangle3 = [](ft_vec3, ft_vec3, ft_vec3, ft_color) {};
    api.get_state = [](ft_engine_state *state) {
      *state = {};
      state->camera = camera;
    };
    void *library = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
    if (!library) throw std::runtime_error(dlerror());
    auto entry = reinterpret_cast<ft_game_module_entry_fn>(dlsym(library, "ft_game_module_entry"));
    const auto *module = entry ? entry(FT_GAME_ABI_VERSION) : nullptr;
    if (!module) throw std::runtime_error("TMNF ABI mismatch");
    auto *game = module->create(&api);
    auto *level = module->level_load_path(game, argv[3], nullptr);
    if (!level) throw std::runtime_error("Cannot load track");
    if (!game->renderer) throw std::runtime_error("TMNF Vulkan renderer did not initialize");
    ft_world_desc world_desc{};
    world_desc.struct_size = sizeof(world_desc);
    world_desc.level = level;
    world_desc.player_count = 1;
    auto *world = module->world_create(game, &world_desc);
    if (!world) throw std::runtime_error("Cannot create car");
    ft_vec3 target{float(level->start.car.position.x), float(level->start.car.position.y),
                   float(level->start.car.position.z)};
    if (argc > 5 && std::strcmp(argv[5], "water") == 0) {
      bool found = false;
      float nearest = INFINITY;
      for (const auto *triangles : {&level->track, &level->translucent})
        for (const auto &triangle : *triangles)
          if (triangle.material < level->materials.size() && level->materials[triangle.material].style.water) {
            auto center = tmnf::Scale(tmnf::Add(tmnf::Add(triangle.a, triangle.b), triangle.c), 1.f / 3);
            float distance = tmnf::LengthSq(tmnf::Sub(center, target));
            if (distance < nearest) {
              nearest = distance;
              camera.target = center;
              found = true;
            }
          }
      if (!found) throw std::runtime_error("Track has no water");
      target = camera.target;
    }
    if (argc > 5 && std::strcmp(argv[5], "checkpoint") == 0) {
      if (level->checkpoints.empty()) throw std::runtime_error("Track has no checkpoints");
      const auto &p = level->checkpoints.back().position;
      target = {p.x, p.y, p.z};
    }
    ft_vec3 eye = tmnf::Add(target, {24, 12, 32});
    const std::string mode = argc > 5 ? argv[5] : "start";
    if (mode != "water" && mode != "checkpoint" && mode != "start" && mode != "chase" &&
        std::sscanf(argv[5], "%f,%f,%f,%f,%f,%f", &eye.x, &eye.y, &eye.z, &target.x, &target.y, &target.z) != 6)
      throw std::runtime_error("Invalid camera");
    View(eye, target);
    if (mode == "chase") {
      ft_camera_frame camera_frame{};
      camera_frame.struct_size = sizeof(camera_frame);
      camera_frame.world = world;
      camera_frame.alpha = 1;
      module->camera_update(game, &camera_frame, &camera);
      if (!game->race_session || !camera.use_view_proj)
        throw std::runtime_error("Original race camera was not selected");
      eye = camera.eye;
      target = camera.target;
    }
    std::fprintf(stderr, "Camera %.3f %.3f %.3f -> %.3f %.3f %.3f\n", eye.x, eye.y, eye.z, target.x, target.y,
                 target.z);
    ft_render_frame frame{};
    frame.struct_size = sizeof(frame);
    frame.level = level;
    frame.opacity = frame.alpha = 1;
    frame.tick = argc > 6 ? std::atoi(argv[6]) : 0;
    frame.state.camera = camera;
    ft_world *ghost = nullptr;
    if (argc > 7 && std::strcmp(argv[7], "exercise") == 0) {
      ghost = module->world_create(game, &world_desc);
      if (!ghost) throw std::runtime_error("Cannot create ghost");
      module->world_copy(game, ghost, world);
      ghost->view.car.position.z += 3;
    }
    double phases[3]{};
    const auto render = [&] {
      const auto before = captures;
      auto start = std::chrono::steady_clock::now();
      frame.pass = FT_PASS_LEVEL_BACKGROUND;
      module->render(game, &frame);
      auto end = std::chrono::steady_clock::now();
      phases[0] += std::chrono::duration<double, std::milli>(end - start).count();
      start = end;
      frame.pass = FT_PASS_ENTITIES;
      frame.world = world;
      module->render(game, &frame);
      if (ghost) {
        frame.world = ghost;
        frame.opacity = .35f;
        module->render(game, &frame);
        ghost->view.car.position.x += 4;
        module->render(game, &frame);
        ghost->view.car.position.x -= 4;
        frame.opacity = 1;
      }
      frame.world = nullptr;
      end = std::chrono::steady_clock::now();
      phases[1] += std::chrono::duration<double, std::milli>(end - start).count();
      start = end;
      frame.pass = FT_PASS_LEVEL_FOREGROUND;
      module->render(game, &frame);
      end = std::chrono::steady_clock::now();
      phases[2] += std::chrono::duration<double, std::milli>(end - start).count();
      if (captures != before + 1) throw std::runtime_error("TMNF did not present exactly one frame");
    };
    for (int i = 0; i < 3; ++i) {
      if (i == 2) output = argv[4];
      render();
    }
    output.clear();
    if (argc > 7 && std::strcmp(argv[7], "exercise") == 0) {
      for (auto size : std::vector<ft_vec2>{{640, 480}, {2304, 720}, {800, 1152}, {1280, 720}}) {
        View(eye, target, uint32_t(size.x), uint32_t(size.y));
        frame.state.camera = camera;
        render();
        render();
      }
      game->settings.draw_collision = true;
      render();
      game->settings.draw_collision = false;
      game->settings.draw_track = false;
      render();
      game->settings.draw_track = true;
      game->settings.draw_background = false;
      render();
      game->settings.draw_background = true;
      module->world_destroy(game, ghost);
      ghost = nullptr;
      module->world_destroy(game, world);
      module->level_destroy(game, level);
      level = module->level_load_path(game, argc > 8 ? argv[8] : argv[3], nullptr);
      if (!level) throw std::runtime_error("Cannot reload track");
      world_desc.level = level;
      world = module->world_create(game, &world_desc);
      frame.level = level;
      render();
      render();
    }
    if (argc > 7 && std::strcmp(argv[7], "bench") == 0) {
      std::fill(phases, phases + 3, 0.0);
      queue_wait_ms = 0;
      const auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < 120; ++i) {
        frame.tick = i;
        render();
      }
      const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 120;
      std::fprintf(stderr, "TMNF render + queue wait: %.3f ms/frame (120 frames)\n", ms);
      std::fprintf(stderr, "Phases: background %.3f, car %.3f, Vulkan + queue wait %.3f ms\n", phases[0] / 120,
                   phases[1] / 120, phases[2] / 120);
      std::fprintf(stderr, "Queue wait: %.3f ms/frame\n", queue_wait_ms / 120);
    }
    module->world_destroy(game, world);
    module->level_destroy(game, level);
    module->destroy(game);
    dlclose(library);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    if (messenger)
      reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"))(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
    return validation_errors ? 1 : 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
