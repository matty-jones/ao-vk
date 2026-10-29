// What the GPU's driver makes of rvk's shaders: builds a pipeline from compiled stages (the build's rvk-shaders/*.inc,
// glslc -mfmt=num) natively on Linux and prints the driver's statistics for each stage (VK_KHR_pipeline_executable_
// properties: on NVIDIA its register count, binary size; on AMD its VGPRs, SGPRs, LDS) - register pressure limits how
// many pixels a shader core keeps in flight, so an uber-shader's worst path slows its every draw.
// The layout comes from the stages themselves (SPIRV-Reflect, from the Vulkan SDK); vertex inputs too.
// Build: tools/shader-stats.sh. Usage: shader-stats [--colour N] stage=FILE.inc ... (stage: vert tesc tese frag comp)
#include <spirv_reflect.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<uint32_t> LoadInc(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    std::vector<uint32_t> words;
    std::string s = ss.str();
    for (char& c : s)
        if (c == ',') c = ' ';
    std::istringstream in(s);
    std::string token;
    while (in >> token) words.push_back(uint32_t(std::stoull(token, nullptr, 0)));   // (0x... words)
    return words;
}

VkShaderStageFlagBits StageOf(const std::string& s)
{
    if (s == "vert") return VK_SHADER_STAGE_VERTEX_BIT;
    if (s == "tesc") return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    if (s == "tese") return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    if (s == "frag") return VK_SHADER_STAGE_FRAGMENT_BIT;
    if (s == "comp") return VK_SHADER_STAGE_COMPUTE_BIT;
    std::fprintf(stderr, "unknown stage %s\n", s.c_str());
    std::exit(2);
}

VkFormat InputFormat(const SpvReflectInterfaceVariable* v)
{
    const uint32_t n = v->numeric.vector.component_count ? v->numeric.vector.component_count : 1;
    const bool isInt = v->type_description->type_flags & SPV_REFLECT_TYPE_FLAG_INT;
    const bool isSigned = v->numeric.scalar.signedness;
    static const VkFormat f[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                                  VK_FORMAT_R32G32B32A32_SFLOAT};
    static const VkFormat u[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT,
                                  VK_FORMAT_R32G32B32A32_UINT};
    static const VkFormat i[4] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT,
                                  VK_FORMAT_R32G32B32A32_SINT};
    return isInt ? (isSigned ? i[n - 1] : u[n - 1]) : f[n - 1];
}

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "%s: %d\n", #x, int(r_)); std::exit(1); } } while (0)

}  // namespace

int main(int argc, char** argv)
{
    uint32_t colours = 5;
    struct Stage { VkShaderStageFlagBits stage; std::vector<uint32_t> code; std::string name; };
    std::vector<Stage> stages;
    for (int a = 1; a < argc; ++a) {
        std::string s = argv[a];
        if (s == "--colour" && a + 1 < argc) { colours = uint32_t(std::atoi(argv[++a])); continue; }
        const size_t eq = s.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "usage: %s [--colour N] stage=FILE.inc ...\n", argv[0]); return 2; }
        stages.push_back({StageOf(s.substr(0, eq)), LoadInc(s.substr(eq + 1)), s.substr(eq + 1)});
        if (stages.back().code.empty()) { std::fprintf(stderr, "%s: no words\n", s.c_str()); return 1; }
    }
    if (stages.empty()) return 2;

    // Instance and device: every core feature the GPU has, on (the shaders use descriptor indexing, ray queries...).
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(instance, &n, pds.data());
    VkPhysicalDevice pd = pds.at(0);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, exts.data());
    auto has = [&](const char* e) {
        for (auto& x : exts) if (!std::strcmp(x.extensionName, e)) return true;
        return false;
    };
    std::vector<const char*> enable = {VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME};
    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pe{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    void** tail = &f2.pNext;
    auto chain = [&](void* s) { *tail = s; tail = reinterpret_cast<void**>(&static_cast<VkBaseOutStructure*>(s)->pNext); };
    chain(&f11); chain(&f12); chain(&f13); chain(&pe);
    if (has(VK_KHR_RAY_QUERY_EXTENSION_NAME) && has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
        has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME)) {
        enable.insert(enable.end(), {VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                     VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME});
        chain(&rq); chain(&as);
    }
    if (has(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) {
        enable.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        chain(&fsr);
    }
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;
    dci.enabledExtensionCount = uint32_t(enable.size());
    dci.ppEnabledExtensionNames = enable.data();
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, nullptr, &dev));

    // The layout, merged over the stages: each binding's type, count (a runtime array: 4096) and stages.
    struct Binding { VkDescriptorType type; uint32_t count; VkShaderStageFlags stages; };
    std::map<uint32_t, std::map<uint32_t, Binding>> sets;
    std::vector<VkPushConstantRange> pushes;
    std::vector<VkVertexInputAttributeDescription> attrs;
    uint32_t stride = 0;
    std::vector<VkShaderModule> modules;
    std::vector<VkPipelineShaderStageCreateInfo> infos;
    std::vector<SpvReflectShaderModule> reflected(stages.size());
    for (size_t s = 0; s < stages.size(); ++s) {
        Stage& st = stages[s];
        SpvReflectShaderModule& m = reflected[s];
        const SpvReflectResult rr = spvReflectCreateShaderModule(st.code.size() * 4, st.code.data(), &m);
        if (rr != SPV_REFLECT_RESULT_SUCCESS) {
            std::fprintf(stderr, "%s: reflection failed (%d)\n", st.name.c_str(), int(rr));
            return 1;
        }
        uint32_t nb = 0;
        spvReflectEnumerateDescriptorBindings(&m, &nb, nullptr);
        std::vector<SpvReflectDescriptorBinding*> bs(nb);
        spvReflectEnumerateDescriptorBindings(&m, &nb, bs.data());
        for (auto* b : bs) {
            uint32_t count = 1;
            for (uint32_t d = 0; d < b->array.dims_count; ++d) count *= b->array.dims[d] ? b->array.dims[d] : 4096;
            if (b->type_description && (b->type_description->op == SpvOpTypeRuntimeArray)) count = 4096;
            auto [it, fresh] = sets[b->set].emplace(b->binding, Binding{VkDescriptorType(b->descriptor_type), count, 0u});
            it->second.stages |= st.stage;
            if (count > it->second.count) it->second.count = count;
        }
        uint32_t np = 0;
        spvReflectEnumeratePushConstantBlocks(&m, &np, nullptr);
        std::vector<SpvReflectBlockVariable*> ps(np);
        spvReflectEnumeratePushConstantBlocks(&m, &np, ps.data());
        for (auto* p : ps) pushes.push_back({VkShaderStageFlags(st.stage), p->offset, p->size});
        if (st.stage == VK_SHADER_STAGE_VERTEX_BIT) {
            uint32_t ni = 0;
            spvReflectEnumerateInputVariables(&m, &ni, nullptr);
            std::vector<SpvReflectInterfaceVariable*> ins(ni);
            spvReflectEnumerateInputVariables(&m, &ni, ins.data());
            for (auto* v : ins) {
                if (v->built_in != -1 || v->location == ~0u) continue;
                attrs.push_back({v->location, 0, InputFormat(v), stride});
                stride += 16;
            }
        }
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = st.code.size() * 4;
        mci.pCode = st.code.data();
        VkShaderModule mod;
        CHECK(vkCreateShaderModule(dev, &mci, nullptr, &mod));
        modules.push_back(mod);
        infos.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, st.stage, mod,
                         m.entry_point_name, nullptr});
    }
    // (Push constants: one range a stage; overlapping ranges with different stages are allowed.)
    uint32_t maxSet = 0;
    for (auto& [set, b] : sets) maxSet = std::max(maxSet, set + 1);
    std::vector<VkDescriptorSetLayout> layouts(maxSet);
    for (uint32_t set = 0; set < maxSet; ++set) {
        std::vector<VkDescriptorSetLayoutBinding> lb;
        std::vector<VkDescriptorBindingFlags> flags;
        for (auto& [binding, b] : sets[set]) {
            lb.push_back({binding, b.type, b.count, b.stages, nullptr});
            flags.push_back(b.count > 1 ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0);
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = uint32_t(flags.size());
        bf.pBindingFlags = flags.data();
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.pNext = &bf;
        lci.bindingCount = uint32_t(lb.size());
        lci.pBindings = lb.data();
        CHECK(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &layouts[set]));
    }
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = maxSet;
    plci.pSetLayouts = layouts.data();
    plci.pushConstantRangeCount = uint32_t(pushes.size());
    plci.pPushConstantRanges = pushes.data();
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(dev, &plci, nullptr, &layout));

    VkPipeline pipeline;
    if (stages.size() == 1 && stages[0].stage == VK_SHADER_STAGE_COMPUTE_BIT) {
        VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cci.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        cci.stage = infos[0];
        cci.layout = layout;
        CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cci, nullptr, &pipeline));
    } else {
        bool tess = false;
        for (auto& st : stages) tess = tess || st.stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        VkVertexInputBindingDescription vb{0, std::max(stride, 4u), VK_VERTEX_INPUT_RATE_VERTEX};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = attrs.empty() ? 0 : 1;
        vi.pVertexBindingDescriptions = &vb;
        vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
        vi.pVertexAttributeDescriptions = attrs.data();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = tess ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineTessellationStateCreateInfo ts{VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
        ts.patchControlPoints = 3;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        std::vector<VkPipelineColorBlendAttachmentState> att(colours);
        for (auto& a : att) a.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = colours;
        cb.pAttachments = att.data();
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dsi.dynamicStateCount = 2;
        dsi.pDynamicStates = dyn;
        std::vector<VkFormat> formats(colours, VK_FORMAT_R16G16B16A16_SFLOAT);
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = colours;
        ri.pColorAttachmentFormats = formats.data();
        ri.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gci.pNext = &ri;
        gci.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        gci.stageCount = uint32_t(infos.size());
        gci.pStages = infos.data();
        gci.pVertexInputState = &vi;
        gci.pInputAssemblyState = &ia;
        gci.pTessellationState = tess ? &ts : nullptr;
        gci.pViewportState = &vp;
        gci.pRasterizationState = &rs;
        gci.pMultisampleState = &ms;
        gci.pDepthStencilState = &ds;
        gci.pColorBlendState = &cb;
        gci.pDynamicState = &dsi;
        gci.layout = layout;
        CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, nullptr, &pipeline));
    }

    auto getProps = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
        vkGetDeviceProcAddr(dev, "vkGetPipelineExecutablePropertiesKHR"));
    auto getStats = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
        vkGetDeviceProcAddr(dev, "vkGetPipelineExecutableStatisticsKHR"));
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = pipeline;
    uint32_t ne = 0;
    getProps(dev, &pi, &ne, nullptr);
    std::vector<VkPipelineExecutablePropertiesKHR> ep(ne, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    getProps(dev, &pi, &ne, ep.data());
    std::printf("%s\n", props.deviceName);
    for (uint32_t e = 0; e < ne; ++e) {
        std::printf("  %s (subgroup %u):", ep[e].name, ep[e].subgroupSize);
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = pipeline;
        ei.executableIndex = e;
        uint32_t ns = 0;
        getStats(dev, &ei, &ns, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> st(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        getStats(dev, &ei, &ns, st.data());
        for (auto& s : st) {
            std::printf(" %s ", s.name);
            switch (s.format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: std::printf("%s", s.value.b32 ? "yes" : "no"); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: std::printf("%lld", (long long)s.value.i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: std::printf("%llu", (unsigned long long)s.value.u64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: std::printf("%.2f", s.value.f64); break;
            default: break;
            }
            std::printf(";");
        }
        std::printf("\n");
    }
    for (auto& m : reflected) spvReflectDestroyShaderModule(&m);
    vkDestroyPipeline(dev, pipeline, nullptr);
    vkDestroyPipelineLayout(dev, layout, nullptr);
    for (auto l : layouts) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    for (auto m : modules) vkDestroyShaderModule(dev, m, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
