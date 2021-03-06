#include "render.h"
#include "coal/m_math.h"
#include "coal/util.h"
#include "tanto/r_geo.h"
#include "tanto/v_image.h"
#include "tanto/v_memory.h"
#include <memory.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <tanto/r_render.h>
#include <tanto/v_video.h>
#include <tanto/t_def.h>
#include <tanto/u_ui.h>
#include <tanto/t_utils.h>
#include <tanto/r_pipeline.h>
#include <tanto/r_raytrace.h>
#include <tanto/r_renderpass.h>
#include <tanto/r_renderpass.h>
#include <tanto/v_command.h>
#include <vulkan/vulkan_core.h>

#define SPVDIR "./shaders/spv"

typedef Tanto_V_Command Command;

static Tanto_V_Image renderTargetDepth;

static VkRenderPass  renderpass;
static VkFramebuffer framebuffers[TANTO_FRAME_COUNT];
static VkPipeline    mainPipeline;

#define MAX_PRIM_COUNT 16

static Tanto_V_BufferRegion cameraBuffers[TANTO_FRAME_COUNT];
static Tanto_V_BufferRegion xformsBuffers[TANTO_FRAME_COUNT];
static Tanto_V_BufferRegion lightsBuffers[TANTO_FRAME_COUNT];

static uint8_t cameraNeedUpdate;
static uint8_t framesNeedUpdate;
static uint8_t xformsNeedUpdate;
static uint8_t lightsNeedUpdate;
static uint8_t texturesNeedUpdate;

typedef Tanto_S_Light Light;

typedef struct {
    Light light[TANTO_S_MAX_LIGHTS];
} Lights;

typedef struct {
    Mat4 xform[MAX_PRIM_COUNT];
} Xforms;

typedef struct {
    Mat4 view;
    Mat4 proj;
    Mat4 camera;
} Camera;

typedef struct {
    Vec4 vec4_0;
    Vec4 vec4_1;
} PushConstant;

static const Tanto_S_Scene* scene;

static Command renderCommands[TANTO_FRAME_COUNT];

static VkDescriptorSetLayout descriptorSetLayouts[TANTO_MAX_DESCRIPTOR_SETS];
static Tanto_R_Description   description[TANTO_FRAME_COUNT];

static VkPipelineLayout pipelineLayout;

typedef enum {
    PIPE_LAYOUT_MAIN,
} PipelineLayoutId;

typedef enum {
    DESC_SET_MAIN,
} DescriptorSetId;

static uint32_t graphicsQueueFamilyIndex;

// declarations for overview and navigation
static void initAttachments(void);
static void initRenderPass(void);
static void initFramebuffers(void);
static void initDescriptorSetsAndPipelineLayouts(void);
static void initPipelines(void);
static void updateDescriptors(void);
static void mainRender(const VkCommandBuffer cmdBuf, const uint32_t frameIndex);
static void updateRenderCommands(const uint32_t frameIndex);
static void onSwapchainRecreate(void);
static void updateLight(uint32_t frameIndex, uint32_t lightIndex);
static void updateCamera(uint32_t index);
static void updateXform(uint32_t frameIndex, uint32_t primIndex);
static void syncScene(const uint32_t frameIndex);

static void initAttachments(void)
{
    renderTargetDepth = tanto_v_CreateImage(
        TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT,
        tanto_r_GetDepthFormat(),
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_SAMPLE_COUNT_1_BIT,
        1,
        graphicsQueueFamilyIndex);
}

static void initRenderPass(void)
{
    tanto_r_CreateRenderPass_ColorDepth(VK_ATTACHMENT_LOAD_OP_CLEAR, 
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 
            tanto_r_GetSwapFormat(), tanto_r_GetDepthFormat(), 
            &renderpass);
}

static void initFramebuffers(void)
{
    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        Tanto_R_Frame* frame = tanto_r_GetFrame(i);
        const VkImageView attachments[] = {
            frame->swapImage.view, renderTargetDepth.view
        };

        const VkFramebufferCreateInfo fbi = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext = NULL,
            .flags = 0,
            .renderPass = renderpass,
            .attachmentCount = 2,
            .pAttachments = attachments,
            .width = TANTO_WINDOW_WIDTH,
            .height = TANTO_WINDOW_HEIGHT,
            .layers = 1,
        };

        V_ASSERT( vkCreateFramebuffer(device, &fbi, NULL, &framebuffers[i]) );
    }
}

static void initDescriptorSetsAndPipelineLayouts(void)
{
    const Tanto_R_DescriptorSetInfo descriptorSets[] = {{
        .bindingCount = 4,
        .bindings = {{ 
            // camera
            .descriptorCount = 1,
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        },{ // xforms
            .descriptorCount = 1,
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT
        },{ // lights
            .descriptorCount = 1,
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
        },{ // textures
            .descriptorCount = TANTO_S_MAX_TEXTURES, // because this is an array of samplers. others are structs of arrays.
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .bindingFlags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
        }}
    }};

    const uint8_t descSetCount = TANTO_ARRAY_SIZE(descriptorSets);
    tanto_r_CreateDescriptorSetLayouts(descSetCount, descriptorSets, descriptorSetLayouts);

    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        tanto_r_CreateDescriptorSets(descSetCount, descriptorSets, descriptorSetLayouts, &description[i]);
    }

    const VkPushConstantRange pcPrimId = {
        .offset = 0,
        .size = sizeof(uint32_t),
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT
    };

    const VkPushConstantRange pcFrag = {
        .offset = sizeof(uint32_t),
        .size = sizeof(uint32_t) * 3 + sizeof(Tanto_S_Material), // we have 12 bytes between the light count and material as padding
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
    };

    const VkPushConstantRange ranges[] = {pcPrimId, pcFrag};

    const Tanto_R_PipelineLayoutInfo pipeLayoutInfos[] = {{
        .descriptorSetCount = 1, 
        .descriptorSetLayouts = descriptorSetLayouts,
        .pushConstantCount = TANTO_ARRAY_SIZE(ranges),
        .pushConstantsRanges = ranges
    }};

    tanto_r_CreatePipelineLayouts(1, pipeLayoutInfos, &pipelineLayout);
}

static void initPipelines(void)
{
    const Tanto_R_GraphicsPipelineInfo graphPipeInfo = {
        .renderPass = renderpass, 
        .layout     = pipelineLayout,
        .sampleCount = VK_SAMPLE_COUNT_1_BIT,
        //.polygonMode = VK_POLYGON_MODE_LINE,
        .frontFace   = VK_FRONT_FACE_CLOCKWISE,
        //.cullMode = VK_CULL_MODE_NONE,
        .vertexDescription = tanto_r_GetVertexDescription(scene->prims[0].rprim.attrCount, scene->prims[0].rprim.attrSizes),
        .vertShader = SPVDIR"/normal-vert.spv",
        .fragShader = SPVDIR"/normal-frag.spv"
    };

    tanto_r_CreateGraphicsPipelines(1, &graphPipeInfo, &mainPipeline);
}

static void updateDescriptors(void)
{
    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        // camera creation
        cameraBuffers[i] = tanto_v_RequestBufferRegion(sizeof(Camera),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);

        // xforms creation
        xformsBuffers[i] = tanto_v_RequestBufferRegion(sizeof(Xforms), 
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);

        // lights creation 
        lightsBuffers[i] = tanto_v_RequestBufferRegion(sizeof(Lights), 
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, TANTO_V_MEMORY_HOST_GRAPHICS_TYPE);

        VkDescriptorBufferInfo camInfo = {
            .buffer = cameraBuffers[i].buffer,
            .offset = cameraBuffers[i].offset,
            .range  = cameraBuffers[i].size
        };

        VkDescriptorBufferInfo xformInfo = {
            .buffer = xformsBuffers[i].buffer,
            .offset = xformsBuffers[i].offset,
            .range  = xformsBuffers[i].size
        };

        VkDescriptorBufferInfo lightInfo = {
            .buffer = lightsBuffers[i].buffer,
            .offset = lightsBuffers[i].offset,
            .range  = lightsBuffers[i].size
        };

        VkWriteDescriptorSet writes[] = {{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description[i].descriptorSets[DESC_SET_MAIN],
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &camInfo
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description[i].descriptorSets[DESC_SET_MAIN],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &xformInfo
        },{
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstArrayElement = 0,
            .dstSet = description[i].descriptorSets[DESC_SET_MAIN],
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &lightInfo
        }};

        vkUpdateDescriptorSets(device, TANTO_ARRAY_SIZE(writes), writes, 0, NULL);
    }
}

static void updateTexture(const uint32_t frameIndex, const Tanto_V_Image* img, const uint32_t texId)
{
    VkDescriptorImageInfo textureInfo = {
        .imageLayout = img->layout,
        .imageView   = img->view,
        .sampler     = img->sampler
    };

    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = NULL,
        .dstSet = description[frameIndex].descriptorSets[DESC_SET_MAIN],
        .dstBinding = 3,
        .dstArrayElement = texId,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &textureInfo
    };

    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    printf("Updated Texture %d frame %d\n", texId, frameIndex);
}

static void mainRender(const VkCommandBuffer cmdBuf, const uint32_t frameIndex)
{
    VkClearValue clearValueColor = {0.002f, 0.001f, 0.009f, 1.0f};
    VkClearValue clearValueDepth = {1.0, 0};

    VkClearValue clears[] = {clearValueColor, clearValueDepth};

    const VkRenderPassBeginInfo rpassInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .clearValueCount = 2,
        .pClearValues = clears,
        .renderArea = {{0, 0}, {TANTO_WINDOW_WIDTH, TANTO_WINDOW_HEIGHT}},
        .renderPass =  renderpass,
        .framebuffer = framebuffers[frameIndex],
    };

    vkCmdBindPipeline(cmdBuf, VK_PIPELINE_BIND_POINT_GRAPHICS, mainPipeline);

    vkCmdBindDescriptorSets(
        cmdBuf, 
        VK_PIPELINE_BIND_POINT_GRAPHICS, 
        pipelineLayout,
        0, 1, &description[frameIndex].descriptorSets[DESC_SET_MAIN],
        0, NULL);

    vkCmdBeginRenderPass(cmdBuf, &rpassInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdPushConstants(cmdBuf, pipelineLayout, 
            VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(uint32_t), sizeof(uint32_t), &scene->lightCount);

    //assert(sizeof(Vec4) == sizeof(Tanto_S_Material));
    assert(scene->primCount < MAX_PRIM_COUNT);

    for (uint32_t p = 0; p < scene->primCount; p++) 
    {
        const Tanto_S_MaterialId matId = scene->prims[p].materialId;
        assert(scene->materials[matId].textureAlbedo != TANTO_S_NONE); // we should hanlde materials without textures with a different pipeline. and possibly sort the prims by pipeline.
        assert(scene->materials[matId].textureRoughness != TANTO_S_NONE); 
        vkCmdPushConstants(cmdBuf, pipelineLayout, 
                VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(uint32_t), &p);
        vkCmdPushConstants(cmdBuf, pipelineLayout, 
                VK_SHADER_STAGE_FRAGMENT_BIT, 16, sizeof(Tanto_S_Material), &scene->materials[matId]);
        tanto_r_DrawPrim(cmdBuf, &scene->prims[p].rprim);
    }

    vkCmdEndRenderPass(cmdBuf);
}

static void updateRenderCommands(const uint32_t frameIndex)
{
    tanto_v_ResetCommand(&renderCommands[frameIndex]);

    VkCommandBuffer cmdBuf = renderCommands[frameIndex].buffer;

    tanto_v_BeginCommandBuffer(cmdBuf);

    mainRender(cmdBuf, frameIndex);

    tanto_v_EndCommandBuffer(cmdBuf);
}

static void onSwapchainRecreate(void)
{
    r_CleanUp();
    initAttachments();
    initPipelines();
    initFramebuffers();
    framesNeedUpdate = TANTO_FRAME_COUNT;
}

static void updateCamera(uint32_t index)
{
    const Mat4 proj = m_BuildPerspective(0.001, 100);
    const Mat4 view = m_Invert4x4(&scene->camera.xform);
    Camera* uboCam = (Camera*)cameraBuffers[index].hostData;
    uboCam->view = view;
    uboCam->proj = proj;
    uboCam->camera = scene->camera.xform;
}

static void updateXform(uint32_t frameIndex, uint32_t primIndex)
{
    Xforms* xforms = (Xforms*)xformsBuffers[frameIndex].hostData;
    xforms->xform[primIndex] = scene->xforms[primIndex];
}

static void updateLight(uint32_t frameIndex, uint32_t lightIndex)
{
    Lights* lights = (Lights*)lightsBuffers[frameIndex].hostData;
    lights->light[lightIndex] = scene->lights[lightIndex];
}

static void syncScene(const uint32_t frameIndex)
{
    if (scene->dirt)
    {
        if (scene->dirt & TANTO_S_CAMERA_BIT)
            cameraNeedUpdate = TANTO_FRAME_COUNT;
        if (scene->dirt & TANTO_S_LIGHTS_BIT)
            lightsNeedUpdate = TANTO_FRAME_COUNT;
        if (scene->dirt & TANTO_S_XFORMS_BIT)
            xformsNeedUpdate = TANTO_FRAME_COUNT;
        if (scene->dirt & TANTO_S_TEXTURES_BIT)
        {
            texturesNeedUpdate = framesNeedUpdate = TANTO_FRAME_COUNT;
        }
    }
    if (cameraNeedUpdate)
    {
        updateCamera(frameIndex);
        cameraNeedUpdate--;
    }
    if (xformsNeedUpdate)
    {
        for (int i = 0; i < scene->primCount; i++) 
            updateXform(frameIndex, i);
        xformsNeedUpdate--;
    }
    if (lightsNeedUpdate)
    {
        for (int i = 0; i < scene->lightCount; i++) 
            updateLight(frameIndex, i);
        lightsNeedUpdate--;
    }
    if (texturesNeedUpdate) // TODO update all tex
    {
        printf("texturesNeedUpdate %d\n", texturesNeedUpdate);
        for (int i = 0; i < scene->textureCount; i++) 
        {
            updateTexture(frameIndex, &scene->textures[i].devImage, i);
        }
        texturesNeedUpdate--;
    }
    if (framesNeedUpdate)
    {
        updateRenderCommands(frameIndex);
        framesNeedUpdate--;
    }
}

void r_InitRenderer(void)
{
    cameraNeedUpdate = TANTO_FRAME_COUNT;
    xformsNeedUpdate = TANTO_FRAME_COUNT;
    framesNeedUpdate = TANTO_FRAME_COUNT;
    texturesNeedUpdate = TANTO_FRAME_COUNT;

    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        renderCommands[i] = tanto_v_CreateCommand(TANTO_V_QUEUE_GRAPHICS_TYPE);
    }

    initAttachments();
    initRenderPass();
    initFramebuffers();
    initDescriptorSetsAndPipelineLayouts();
    updateDescriptors();

    tanto_r_RegisterSwapchainRecreationFn(onSwapchainRecreate);
}

void r_Render(void)
{
    uint32_t f = tanto_r_RequestFrame();
    tanto_v_WaitForFence(&renderCommands[f].fence);
    syncScene(f);
    VkPipelineStageFlags stageFlags = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore* waitSemaphore = NULL;
    tanto_v_SubmitGraphicsCommand(0, &stageFlags, waitSemaphore, renderCommands[f].fence, &renderCommands[f]);
    VkSemaphore* s = tanto_u_Render(&renderCommands[f].semaphore);
    tanto_r_PresentFrame(*s);
}

void r_BindScene(const Tanto_S_Scene* pScene)
{
    printf("Renderer: Scene bound.\n");
    scene = pScene;
    initPipelines();
}

void r_CleanUp(void)
{
    for (int i = 0; i < TANTO_FRAME_COUNT; i++) 
    {
        vkDestroyFramebuffer(device, framebuffers[i], NULL);
    }
    tanto_v_FreeImage(&renderTargetDepth);
    vkDestroyPipeline(device, mainPipeline, NULL);
}
