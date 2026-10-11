/* Reference run of CS_ModelSkinning on Linux: the game's DXIL translated to SPIR-V by
 * dxil-spirv (what vkd3d-proton does on Linux/Steam Deck, where the game animates
 * fine), executed by Mesa's lavapipe on the CPU. Writes the output buffer for
 * skin_gpu_test.exe to compare against.
 *
 *   skin_ref_vk skin.spv golden-skin.bin [seed]
 *
 * skin.spv: dxil-spirv --typed-uav-read-without-format, then set 0 bindings remapped to
 * t0=0 t1=1 t2=2 t97=3 t101=4 u0=5 (see make-reference.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "skin_inputs.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #x, r_); exit(1); } } while (0)

static VkDevice dev;
static VkPhysicalDevice phys;

struct buf { VkBuffer buffer; VkDeviceMemory memory; VkBufferView view; VkDeviceSize size; };

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties p;
    uint32_t i;

    vkGetPhysicalDeviceMemoryProperties(phys, &p);
    for (i = 0; i < p.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "no memory type\n");
    exit(1);
}

static struct buf make_buffer(const void *data, VkDeviceSize size, VkBufferUsageFlags usage, VkFormat format)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkBufferViewCreateInfo vci = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
    VkMemoryRequirements req;
    struct buf b;
    void *map;

    b.size = size;
    bci.size = size;
    bci.usage = usage;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &b.buffer));
    vkGetBufferMemoryRequirements(dev, b.buffer, &req);
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(vkAllocateMemory(dev, &mai, NULL, &b.memory));
    CHECK(vkBindBufferMemory(dev, b.buffer, b.memory, 0));
    CHECK(vkMapMemory(dev, b.memory, 0, size, 0, &map));
    if (data)
        memcpy(map, data, size);
    else
        memset(map, 0, size);
    vkUnmapMemory(dev, b.memory);
    vci.buffer = b.buffer;
    vci.format = format;
    vci.range = VK_WHOLE_SIZE;
    CHECK(vkCreateBufferView(dev, &vci, NULL, &b.view));
    return b;
}

int main(int argc, char **argv)
{
    static struct skin_inputs in;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkPhysicalDeviceFeatures feats = {0};
    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkDescriptorSetLayoutBinding bindings[6];
    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 5 }, { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkWriteDescriptorSet writes[6];
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkInstance inst;
    VkQueue queue;
    VkShaderModule module;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkCommandPool cmdpool;
    VkCommandBuffer cmd;
    struct buf bufs[6];
    uint32_t count = 1, seed = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0, *spv, *out, i, written = 0;
    float prio = 1.0f;
    long spv_size;
    FILE *f;

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s skin.spv golden-skin.bin [seed]\n", argv[0]);
        return 2;
    }
    if (!(f = fopen(argv[1], "rb")))
        return perror(argv[1]), 1;
    fseek(f, 0, SEEK_END);
    spv_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    spv = malloc(spv_size);
    if (fread(spv, 1, spv_size, f) != (size_t)spv_size)
        return 1;
    fclose(f);
    skin_make_inputs(&in, seed);

    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    CHECK(vkEnumeratePhysicalDevices(inst, &count, &phys));
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    feats.shaderStorageImageReadWithoutFormat = VK_TRUE;
    feats.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    feats.robustBufferAccess = VK_TRUE;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &feats;
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, 0, 0, &queue);

    bufs[0] = make_buffer(in.bones, sizeof(in.bones), VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, VK_FORMAT_R32_UINT);
    bufs[1] = make_buffer(in.data, sizeof(in.data), VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, VK_FORMAT_R32_UINT);
    bufs[2] = make_buffer(in.entries, sizeof(in.entries), VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, VK_FORMAT_R32_UINT);
    bufs[3] = make_buffer(in.raw, sizeof(in.raw), VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, VK_FORMAT_R32_UINT);
    bufs[4] = make_buffer(in.inst, sizeof(in.inst), VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, VK_FORMAT_R32_UINT);
    bufs[5] = make_buffer(NULL, (VkDeviceSize)SKIN_OUT_VERTS * 16, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
            VK_FORMAT_R32G32B32A32_UINT);

    smci.codeSize = spv_size;
    smci.pCode = spv;
    CHECK(vkCreateShaderModule(dev, &smci, NULL, &module));
    for (i = 0; i < 6; i++)
    {
        memset(&bindings[i], 0, sizeof(bindings[i]));
        bindings[i].binding = i;
        bindings[i].descriptorType = i == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    dslci.bindingCount = 6;
    dslci.pBindings = bindings;
    CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dsl;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = "main";
    cpci.layout = layout;
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline));

    dpci.maxSets = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &pool));
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &dsl;
    CHECK(vkAllocateDescriptorSets(dev, &dsai, &set));
    for (i = 0; i < 6; i++)
    {
        memset(&writes[i], 0, sizeof(writes[i]));
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = bindings[i].descriptorType;
        writes[i].pTexelBufferView = &bufs[i].view;
    }
    vkUpdateDescriptorSets(dev, 6, writes, 0, NULL);

    cpi.queueFamilyIndex = 0;
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cmdpool));
    cbai.commandPool = cmdpool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cmd, SKIN_GROUPS, 1, 1);
    CHECK(vkEndCommandBuffer(cmd));
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));

    CHECK(vkMapMemory(dev, bufs[5].memory, 0, bufs[5].size, 0, (void **)&out));
    for (i = 0; i < SKIN_OUT_VERTS; i++)
        written += (out[i * 4] | out[i * 4 + 1] | out[i * 4 + 2] | out[i * 4 + 3]) != 0;
    if (!(f = fopen(argv[2], "wb")) || fwrite(out, 16, SKIN_OUT_VERTS, f) != SKIN_OUT_VERTS)
        return perror(argv[2]), 1;
    fclose(f);
    printf("reference: %u of %u output vertices written -> %s\n", written, SKIN_OUT_VERTS, argv[2]);
    return 0;
}
