#include "bench/vulkan_kernel.hpp"

#include <cstring>
#include <format>

#include "platform/dynlib.hpp"

#if GPUOC_HAVE_VULKAN
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

namespace gpuoc {

std::string format_uuid(const std::array<std::uint8_t, 16>& b) {
  return std::format(
      "GPU-{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}", b[0],
      b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

#if !GPUOC_HAVE_VULKAN

struct VulkanKernel::Impl {};
VulkanKernel::VulkanKernel(std::unique_ptr<Impl> impl, VulkanDeviceInfo info, KernelParams params)
    : impl_(std::move(impl)), info_(std::move(info)), params_(params) {}
VulkanKernel::~VulkanKernel() = default;
Result<std::vector<VulkanDeviceInfo>> VulkanKernel::list_devices() {
  return make_error(ErrorCode::Unavailable, "built without Vulkan support");
}
Result<std::unique_ptr<VulkanKernel>> VulkanKernel::create(const DeviceSelector&, const KernelParams&) {
  return make_error(ErrorCode::Unavailable, "built without Vulkan support");
}
KernelRunResult VulkanKernel::run_once(std::chrono::milliseconds) {
  return KernelRunResult{KernelRunResult::Status::Error, 0, 0, "built without Vulkan support"};
}

#else

namespace {

const std::uint32_t kSpirv[] = {
#include "bench/kernel_spv.inc"
};

std::string vk_error(const char* what, VkResult r) { return std::format("{} failed (VkResult {})", what, static_cast<int>(r)); }

struct Loader {
  std::shared_ptr<platform::DynLib> lib;
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  PFN_vkCreateInstance create_instance = nullptr;
  PFN_vkEnumerateInstanceVersion enumerate_version = nullptr;

  static Result<Loader> load() {
    Loader l;
#ifdef _WIN32
    GPUOC_TRY_ASSIGN(l.lib, platform::DynLib::open({"vulkan-1.dll"}));
#else
    GPUOC_TRY_ASSIGN(l.lib, platform::DynLib::open({"libvulkan.so.1", "libvulkan.so"}));
#endif
    l.gipa = l.lib->get<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    if (!l.gipa) return make_error(ErrorCode::Unavailable, "vkGetInstanceProcAddr not exported");
    l.create_instance = reinterpret_cast<PFN_vkCreateInstance>(l.gipa(nullptr, "vkCreateInstance"));
    l.enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(l.gipa(nullptr, "vkEnumerateInstanceVersion"));
    if (!l.create_instance) return make_error(ErrorCode::Unavailable, "vkCreateInstance unavailable");
    return l;
  }
};

#define GPUOC_VK_INSTANCE_FNS(X)              \
  X(vkDestroyInstance)                        \
  X(vkEnumeratePhysicalDevices)               \
  X(vkGetPhysicalDeviceProperties)            \
  X(vkGetPhysicalDeviceProperties2)           \
  X(vkGetPhysicalDeviceQueueFamilyProperties) \
  X(vkGetPhysicalDeviceMemoryProperties)      \
  X(vkCreateDevice)                           \
  X(vkGetDeviceProcAddr)

#define GPUOC_VK_DEVICE_FNS(X)      \
  X(vkDestroyDevice)                \
  X(vkGetDeviceQueue)               \
  X(vkCreateBuffer)                 \
  X(vkDestroyBuffer)                \
  X(vkGetBufferMemoryRequirements)  \
  X(vkAllocateMemory)               \
  X(vkFreeMemory)                   \
  X(vkBindBufferMemory)             \
  X(vkMapMemory)                    \
  X(vkUnmapMemory)                  \
  X(vkCreateShaderModule)           \
  X(vkDestroyShaderModule)          \
  X(vkCreateDescriptorSetLayout)    \
  X(vkDestroyDescriptorSetLayout)   \
  X(vkCreatePipelineLayout)         \
  X(vkDestroyPipelineLayout)        \
  X(vkCreateComputePipelines)       \
  X(vkDestroyPipeline)              \
  X(vkCreateDescriptorPool)         \
  X(vkDestroyDescriptorPool)        \
  X(vkAllocateDescriptorSets)       \
  X(vkUpdateDescriptorSets)         \
  X(vkCreateCommandPool)            \
  X(vkDestroyCommandPool)           \
  X(vkAllocateCommandBuffers)       \
  X(vkBeginCommandBuffer)           \
  X(vkEndCommandBuffer)             \
  X(vkCmdBindPipeline)              \
  X(vkCmdBindDescriptorSets)        \
  X(vkCmdPushConstants)             \
  X(vkCmdDispatch)                  \
  X(vkCmdPipelineBarrier)           \
  X(vkCmdFillBuffer)                \
  X(vkQueueSubmit)                  \
  X(vkCreateFence)                  \
  X(vkDestroyFence)                 \
  X(vkWaitForFences)                \
  X(vkResetFences)                  \
  X(vkDeviceWaitIdle)

struct InstanceFns {
#define X(name) PFN_##name name = nullptr;
  GPUOC_VK_INSTANCE_FNS(X)
#undef X
};

struct DeviceFns {
#define X(name) PFN_##name name = nullptr;
  GPUOC_VK_DEVICE_FNS(X)
#undef X
};

struct PushConstants {
  std::uint32_t n_mask, pass, iters, flags;
};

struct InstanceHandle {
  Loader loader;
  VkInstance instance = VK_NULL_HANDLE;
  InstanceFns fn;
  std::uint32_t api = VK_API_VERSION_1_0;

  static Result<std::unique_ptr<InstanceHandle>> create() {
    auto h = std::make_unique<InstanceHandle>();
    GPUOC_TRY_ASSIGN(h->loader, Loader::load());
    std::uint32_t loader_api = VK_API_VERSION_1_0;
    if (h->loader.enumerate_version) h->loader.enumerate_version(&loader_api);
    h->api = loader_api >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "gpuoc-worker";
    app.applicationVersion = 1;
    app.pEngineName = "gpuoc";
    app.apiVersion = h->api;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    if (VkResult r = h->loader.create_instance(&ci, nullptr, &h->instance); r != VK_SUCCESS)
      return make_error(ErrorCode::Unavailable, vk_error("vkCreateInstance", r));
#define X(name) h->fn.name = reinterpret_cast<PFN_##name>(h->loader.gipa(h->instance, #name));
    GPUOC_VK_INSTANCE_FNS(X)
#undef X
    if (!h->fn.vkDestroyInstance || !h->fn.vkEnumeratePhysicalDevices || !h->fn.vkCreateDevice ||
        !h->fn.vkGetDeviceProcAddr || !h->fn.vkGetPhysicalDeviceProperties)
      return make_error(ErrorCode::Unavailable, "required Vulkan instance functions missing");
    return h;
  }

  ~InstanceHandle() {
    if (instance && fn.vkDestroyInstance) fn.vkDestroyInstance(instance, nullptr);
  }

  Result<std::vector<std::pair<VkPhysicalDevice, VulkanDeviceInfo>>> devices() {
    std::uint32_t n = 0;
    if (VkResult r = fn.vkEnumeratePhysicalDevices(instance, &n, nullptr); r != VK_SUCCESS)
      return make_error(ErrorCode::Unavailable, vk_error("vkEnumeratePhysicalDevices", r));
    std::vector<VkPhysicalDevice> pds(n);
    fn.vkEnumeratePhysicalDevices(instance, &n, pds.data());
    std::vector<std::pair<VkPhysicalDevice, VulkanDeviceInfo>> out;
    for (std::uint32_t i = 0; i < n; ++i) {
      VkPhysicalDeviceProperties p{};
      fn.vkGetPhysicalDeviceProperties(pds[i], &p);
      VulkanDeviceInfo info;
      info.name = p.deviceName;
      info.vendor_id = p.vendorID;
      info.index = i;
      if (api >= VK_API_VERSION_1_1 && p.apiVersion >= VK_API_VERSION_1_1 && fn.vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties idp{};
        idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &idp;
        fn.vkGetPhysicalDeviceProperties2(pds[i], &p2);
        std::array<std::uint8_t, 16> u{};
        std::memcpy(u.data(), idp.deviceUUID, 16);
        info.uuid = format_uuid(u);
      }
      out.emplace_back(pds[i], info);
    }
    return out;
  }
};

std::string lower(std::string s) {
  for (auto& c : s) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  return s;
}

}  // namespace

struct VulkanKernel::Impl {
  std::unique_ptr<InstanceHandle> inst;
  VkPhysicalDevice pd = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  DeviceFns fn;
  VkQueue queue = VK_NULL_HANDLE;
  VkBuffer buf[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkDeviceMemory mem[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
  void* sum_ptr = nullptr;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkCommandPool cmd_pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool lost = false;

  ~Impl() {
    if (!device) return;
    if (!lost && fn.vkDeviceWaitIdle) fn.vkDeviceWaitIdle(device);
    if (fence) fn.vkDestroyFence(device, fence, nullptr);
    if (cmd_pool) fn.vkDestroyCommandPool(device, cmd_pool, nullptr);
    if (pool) fn.vkDestroyDescriptorPool(device, pool, nullptr);
    if (pipeline) fn.vkDestroyPipeline(device, pipeline, nullptr);
    if (layout) fn.vkDestroyPipelineLayout(device, layout, nullptr);
    if (dsl) fn.vkDestroyDescriptorSetLayout(device, dsl, nullptr);
    if (shader) fn.vkDestroyShaderModule(device, shader, nullptr);
    if (sum_ptr) fn.vkUnmapMemory(device, mem[2]);
    for (int i = 0; i < 3; ++i) {
      if (buf[i]) fn.vkDestroyBuffer(device, buf[i], nullptr);
      if (mem[i]) fn.vkFreeMemory(device, mem[i], nullptr);
    }
    fn.vkDestroyDevice(device, nullptr);
  }

  Result<std::uint32_t> memory_type(std::uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback) {
    VkPhysicalDeviceMemoryProperties mp{};
    inst->fn.vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (VkMemoryPropertyFlags flags : {want, fallback}) {
      for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
      }
    }
    return make_error(ErrorCode::Unavailable, "no suitable Vulkan memory type");
  }

  Result<void> make_buffer(int idx, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want,
                           VkMemoryPropertyFlags fallback) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (VkResult r = fn.vkCreateBuffer(device, &bi, nullptr, &buf[idx]); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkCreateBuffer", r));
    VkMemoryRequirements req{};
    fn.vkGetBufferMemoryRequirements(device, buf[idx], &req);
    GPUOC_TRY_ASSIGN(std::uint32_t type, memory_type(req.memoryTypeBits, want, fallback));
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (VkResult r = fn.vkAllocateMemory(device, &ai, nullptr, &mem[idx]); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkAllocateMemory", r));
    if (VkResult r = fn.vkBindBufferMemory(device, buf[idx], mem[idx], 0); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkBindBufferMemory", r));
    return success();
  }

  Result<void> init(std::uint32_t queue_family, const KernelParams& p) {
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(p.elements) * 4;
    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    GPUOC_TRY(make_buffer(0, bytes, storage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0));
    GPUOC_TRY(make_buffer(1, bytes, storage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0));
    GPUOC_TRY(make_buffer(2, 16, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
    if (VkResult r = fn.vkMapMemory(device, mem[2], 0, 16, 0, &sum_ptr); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkMapMemory", r));

    VkShaderModuleCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = sizeof(kSpirv);
    si.pCode = kSpirv;
    if (VkResult r = fn.vkCreateShaderModule(device, &si, nullptr, &shader); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkCreateShaderModule", r));

    VkDescriptorSetLayoutBinding bindings[3]{};
    for (std::uint32_t i = 0; i < 3; ++i) {
      bindings[i].binding = i;
      bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      bindings[i].descriptorCount = 1;
      bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = 3;
    dli.pBindings = bindings;
    if (VkResult r = fn.vkCreateDescriptorSetLayout(device, &dli, nullptr, &dsl); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkCreateDescriptorSetLayout", r));

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if (VkResult r = fn.vkCreatePipelineLayout(device, &pli, nullptr, &layout); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkCreatePipelineLayout", r));

    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = shader;
    cpi.stage.pName = "main";
    cpi.layout = layout;
    if (VkResult r = fn.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkCreateComputePipelines", r));

    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = 6;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 2;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (VkResult r = fn.vkCreateDescriptorPool(device, &dpi, nullptr, &pool); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkCreateDescriptorPool", r));
    VkDescriptorSetLayout layouts[2] = {dsl, dsl};
    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = layouts;
    if (VkResult r = fn.vkAllocateDescriptorSets(device, &dai, sets); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkAllocateDescriptorSets", r));
    for (int s = 0; s < 2; ++s) {
      // Set 0: A -> B. Set 1: B -> A.
      VkDescriptorBufferInfo infos[3]{};
      infos[0].buffer = buf[s == 0 ? 0 : 1];
      infos[1].buffer = buf[s == 0 ? 1 : 0];
      infos[2].buffer = buf[2];
      for (auto& i : infos) i.range = VK_WHOLE_SIZE;
      VkWriteDescriptorSet w[3]{};
      for (std::uint32_t b = 0; b < 3; ++b) {
        w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[b].dstSet = sets[s];
        w[b].dstBinding = b;
        w[b].descriptorCount = 1;
        w[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[b].pBufferInfo = &infos[b];
      }
      fn.vkUpdateDescriptorSets(device, 3, w, 0, nullptr);
    }

    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = queue_family;
    if (VkResult r = fn.vkCreateCommandPool(device, &cpci, nullptr, &cmd_pool); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkCreateCommandPool", r));
    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = cmd_pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    if (VkResult r = fn.vkAllocateCommandBuffers(device, &cbai, &cmd); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkAllocateCommandBuffers", r));

    // Record once; resubmitted for every run.
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    fn.vkBeginCommandBuffer(cmd, &bi);
    fn.vkCmdFillBuffer(cmd, buf[2], 0, 16, 0);
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                            nullptr, 0, nullptr);
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    for (std::uint32_t pass = 0; pass < p.passes; ++pass) {
      fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &sets[pass % 2], 0, nullptr);
      PushConstants pc{p.elements - 1, pass, p.iters,
                       (pass == 0 ? 1u : 0u) | (pass + 1 == p.passes ? 2u : 0u)};
      fn.vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
      fn.vkCmdDispatch(cmd, p.elements / 256, 1, 1);
      VkMemoryBarrier cb{};
      cb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      cb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      cb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                              &cb, 0, nullptr, 0, nullptr);
    }
    VkMemoryBarrier hb{};
    hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0,
                            nullptr, 0, nullptr);
    if (VkResult r = fn.vkEndCommandBuffer(cmd); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkEndCommandBuffer", r));

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (VkResult r = fn.vkCreateFence(device, &fci, nullptr, &fence); r != VK_SUCCESS)
      return make_error(ErrorCode::HardwareError, vk_error("vkCreateFence", r));
    return success();
  }
};

VulkanKernel::VulkanKernel(std::unique_ptr<Impl> impl, VulkanDeviceInfo info, KernelParams params)
    : impl_(std::move(impl)), info_(std::move(info)), params_(params) {}

VulkanKernel::~VulkanKernel() = default;

Result<std::vector<VulkanDeviceInfo>> VulkanKernel::list_devices() {
  GPUOC_TRY_ASSIGN(auto inst, InstanceHandle::create());
  GPUOC_TRY_ASSIGN(auto devs, inst->devices());
  std::vector<VulkanDeviceInfo> out;
  for (auto& d : devs) out.push_back(d.second);
  return out;
}

Result<std::unique_ptr<VulkanKernel>> VulkanKernel::create(const DeviceSelector& sel, const KernelParams& params) {
  GPUOC_TRY(validate(params));
  auto impl = std::make_unique<Impl>();
  GPUOC_TRY_ASSIGN(impl->inst, InstanceHandle::create());
  GPUOC_TRY_ASSIGN(auto devs, impl->inst->devices());
  if (devs.empty()) return make_error(ErrorCode::Unavailable, "no Vulkan devices");

  const std::pair<VkPhysicalDevice, VulkanDeviceInfo>* chosen = nullptr;
  if (!sel.uuid.empty()) {
    for (const auto& d : devs) {
      if (lower(d.second.uuid) == lower(sel.uuid)) chosen = &d;
    }
    if (!chosen) return make_error(ErrorCode::NotFound, std::format("no Vulkan device with UUID {}", sel.uuid));
  } else if (sel.index) {
    if (*sel.index < 0 || static_cast<std::size_t>(*sel.index) >= devs.size())
      return make_error(ErrorCode::NotFound, std::format("no Vulkan device at index {}", *sel.index));
    chosen = &devs[static_cast<std::size_t>(*sel.index)];
  } else {
    for (const auto& d : devs) {
      if (sel.prefer_nvidia && d.second.vendor_id == 0x10DE) {
        chosen = &d;
        break;
      }
    }
    if (!chosen) chosen = &devs.front();
  }
  impl->pd = chosen->first;

  std::uint32_t qn = 0;
  impl->inst->fn.vkGetPhysicalDeviceQueueFamilyProperties(impl->pd, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qn);
  impl->inst->fn.vkGetPhysicalDeviceQueueFamilyProperties(impl->pd, &qn, qf.data());
  std::optional<std::uint32_t> family;
  for (std::uint32_t i = 0; i < qn; ++i) {
    if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      family = i;
      if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) break;  // prefer a dedicated compute queue
    }
  }
  if (!family) return make_error(ErrorCode::Unavailable, "device has no compute queue");

  const float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = *family;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  if (VkResult r = impl->inst->fn.vkCreateDevice(impl->pd, &dci, nullptr, &impl->device); r != VK_SUCCESS)
    return make_error(ErrorCode::HardwareError, vk_error("vkCreateDevice", r));
#define X(name) \
  impl->fn.name = reinterpret_cast<PFN_##name>(impl->inst->fn.vkGetDeviceProcAddr(impl->device, #name));
  GPUOC_VK_DEVICE_FNS(X)
#undef X
#define X(name) \
  if (!impl->fn.name) return make_error(ErrorCode::Unavailable, "Vulkan device function missing: " #name);
  GPUOC_VK_DEVICE_FNS(X)
#undef X
  impl->fn.vkGetDeviceQueue(impl->device, *family, 0, &impl->queue);
  GPUOC_TRY(impl->init(*family, params));
  VulkanDeviceInfo info = chosen->second;
  return std::unique_ptr<VulkanKernel>(new VulkanKernel(std::move(impl), std::move(info), params));
}

KernelRunResult VulkanKernel::run_once(std::chrono::milliseconds timeout) {
  KernelRunResult out;
  auto& d = *impl_;
  if (d.lost) {
    out.status = KernelRunResult::Status::DeviceLost;
    out.detail = "device previously lost";
    return out;
  }
  const auto t0 = std::chrono::steady_clock::now();
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &d.cmd;
  VkResult r = d.fn.vkQueueSubmit(d.queue, 1, &si, d.fence);
  if (r == VK_SUCCESS) {
    r = d.fn.vkWaitForFences(d.device, 1, &d.fence, VK_TRUE,
                             static_cast<std::uint64_t>(std::chrono::nanoseconds(timeout).count()));
  }
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (r == VK_TIMEOUT) {
    out.status = KernelRunResult::Status::Timeout;
    out.detail = std::format("kernel did not finish within {} ms", timeout.count());
    d.lost = true;  // cannot safely reuse the fence
    return out;
  }
  if (r == VK_ERROR_DEVICE_LOST) {
    out.status = KernelRunResult::Status::DeviceLost;
    out.detail = "VK_ERROR_DEVICE_LOST";
    d.lost = true;
    return out;
  }
  if (r != VK_SUCCESS) {
    out.status = KernelRunResult::Status::Error;
    out.detail = vk_error("vkQueueSubmit/vkWaitForFences", r);
    return out;
  }
  d.fn.vkResetFences(d.device, 1, &d.fence);
  std::uint32_t sums[2];
  std::memcpy(sums, d.sum_ptr, sizeof sums);
  out.checksum = (static_cast<std::uint64_t>(sums[0]) << 32) | sums[1];
  return out;
}

#endif  // GPUOC_HAVE_VULKAN

}  // namespace gpuoc
