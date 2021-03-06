// Copyright 2017 The Dawn Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "SampleUtils.h"
#include "utils/SystemUtils.h"
#include "utils/WGPUHelpers.h"

WGPUDevice device;
WGPUQueue queue;
WGPUSwapChain swapchain;

WGPURenderPipeline pipeline;
WGPUBindGroupLayout bindGroupLayout;
WGPUBindGroup bindGroup;

WGPUBuffer vertexBuffer;
WGPUBuffer indexBuffer;

WGPUBuffer pixelBuffer;

WGPUShaderModule vsModule;
WGPUShaderModule fsModule;
WGPUShaderModule rayGenModule;
WGPUShaderModule rayCHitModule;
WGPUShaderModule rayMissModule;

WGPUTextureFormat swapChainFormat;

WGPURayTracingAccelerationContainer geometryContainer;
WGPURayTracingAccelerationContainer instanceContainer;

WGPUBindGroupLayout rtBindGroupLayout;
WGPUBindGroup rtBindGroup;

WGPUPipelineLayout rtPipelineLayout;
WGPURayTracingPipeline rtPipeline;

uint32_t width = 640;
uint32_t height = 480;

uint64_t pixelBufferSize = width * height * 4 * sizeof(float);

void init() {
    std::vector<const char*> requiredExtensions = {"ray_tracing"};
    device = CreateCppDawnDevice(wgpu::BackendType::D3D12, requiredExtensions).Release();
    queue = wgpuDeviceGetDefaultQueue(device);

    {
        WGPUSwapChainDescriptor descriptor;
        descriptor.nextInChain = nullptr;
        descriptor.label = nullptr;
        descriptor.implementation = GetSwapChainImplementation();
        swapchain = wgpuDeviceCreateSwapChain(device, nullptr, &descriptor);
    }
    swapChainFormat = static_cast<WGPUTextureFormat>(GetPreferredSwapChainTextureFormat());
    wgpuSwapChainConfigure(swapchain, swapChainFormat, WGPUTextureUsage_OutputAttachment, width,
                           height);

    const char* rayGen = R"(
        #version 460
        #extension GL_EXT_ray_tracing  : require

        layout(location = 0) rayPayloadEXT vec3 payload;

        layout(set = 0, binding = 0) uniform accelerationStructureEXT topLevelAS;
        layout(set = 0, binding = 1, std140) buffer PixelBuffer {
            vec4 pixels[];
        } pixelBuffer;

        void main() {
            const vec2 pixelCenter = vec2(gl_LaunchIDEXT.xy) + vec2(0.5);
            const vec2 uv = pixelCenter / vec2(gl_LaunchSizeEXT.xy);
            const vec2 d = uv * 2.0 - 1.0;
            const float aspectRatio = float(gl_LaunchSizeEXT.x) / float(gl_LaunchSizeEXT.y);
            const vec3 origin = vec3(0, 0, -1.5);
            const vec3 direction = normalize(vec3(d.x * aspectRatio, d.y, 1));
            payload = vec3(0);
            traceRayEXT(topLevelAS, gl_RayFlagsOpaqueEXT, 0xff, 0, 0, 0, origin, 0.001, direction, 100.0, 0 );
            const uint pixelIndex = (gl_LaunchSizeEXT.y - gl_LaunchIDEXT.y) * gl_LaunchSizeEXT.x + gl_LaunchIDEXT.x;
            pixelBuffer.pixels[pixelIndex] = vec4(payload, 1.0);
        }
    )";

    const char* rayCHit = R"(
        #version 460 core
        #extension GL_EXT_ray_tracing : enable

        layout(location = 0) rayPayloadInEXT vec3 payload;

        hitAttributeEXT vec2 attribs;

        void main() {
            vec3 bary = vec3(
                1.0 - attribs.x - attribs.y,
                attribs.x,
                attribs.y
            );
            payload = bary;
        }
    )";

    const char* rayMiss = R"(
        #version 460 core
        #extension GL_EXT_ray_tracing : enable

        layout(location = 0) rayPayloadInEXT vec3 payload;

        void main() {
            payload = vec3(0.15);
        }
    )";

    const char* vs = R"(
        #version 460
        layout (location = 0) out vec2 uv;
        void main() {
            vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
            gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
            uv = pos;
        }
    )";

    const char* fs = R"(
        #version 460
        layout (location = 0) in vec2 uv;
        layout (location = 0) out vec4 outColor;
        layout(std140, set = 0, binding = 0) buffer PixelBuffer {
            vec4 pixels[];
        } pixelBuffer;
        const vec2 resolution = vec2(640, 480);
        void main() {
            const ivec2 bufferCoord = ivec2(floor(uv * resolution));
            const vec2 fragCoord = (uv * resolution);
            const uint pixelIndex = bufferCoord.y * uint(resolution.x) + bufferCoord.x;
            vec4 pixelColor = pixelBuffer.pixels[pixelIndex];
            outColor = pixelColor;
        }
    )";

    vsModule = utils::CreateShaderModule(wgpu::Device(device), utils::SingleShaderStage::Vertex, vs)
                   .Release();

    fsModule = utils::CreateShaderModule(device, utils::SingleShaderStage::Fragment, fs).Release();

    rayGenModule =
        utils::CreateShaderModule(device, utils::SingleShaderStage::RayGeneration, rayGen)
            .Release();

    rayCHitModule =
        utils::CreateShaderModule(device, utils::SingleShaderStage::RayClosestHit, rayCHit)
            .Release();

    rayMissModule =
        utils::CreateShaderModule(device, utils::SingleShaderStage::RayMiss, rayMiss).Release();

    // clang-format off
    const float vertexData[] = {
         1.0f,  1.0f,  0.0f,
        -1.0f,  1.0f,  0.0f,
         0.0f, -1.0f,  0.0f
    };
    // clang-format on
    {
        WGPUBufferDescriptor descriptor;
        descriptor.label = nullptr;
        descriptor.nextInChain = nullptr;
        descriptor.size = sizeof(vertexData);
        descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_RayTracing;

        vertexBuffer = wgpuDeviceCreateBuffer(device, &descriptor);
        wgpuBufferSetSubData(vertexBuffer, 0, sizeof(vertexData), vertexData);
    }

    // clang-format off
    const uint32_t indexData[] = {
        0, 1, 2
    };
    // clang-format on
    {
        WGPUBufferDescriptor descriptor;
        descriptor.label = nullptr;
        descriptor.nextInChain = nullptr;
        descriptor.size = sizeof(indexData);
        descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_RayTracing;

        indexBuffer = wgpuDeviceCreateBuffer(device, &descriptor);
        wgpuBufferSetSubData(indexBuffer, 0, sizeof(indexData), indexData);
    }

    {
        WGPUBufferDescriptor descriptor;
        descriptor.label = nullptr;
        descriptor.nextInChain = nullptr;
        descriptor.size = pixelBufferSize;
        descriptor.usage = WGPUBufferUsage_Storage;

        pixelBuffer = wgpuDeviceCreateBuffer(device, &descriptor);
    }

    {
        WGPURayTracingAccelerationGeometryVertexDescriptor vertexDescriptor;
        vertexDescriptor.offset = 0;
        vertexDescriptor.buffer = vertexBuffer;
        vertexDescriptor.format = WGPUVertexFormat_Float3;
        vertexDescriptor.stride = 3 * sizeof(float);
        vertexDescriptor.count = sizeof(vertexData) / sizeof(float);

        WGPURayTracingAccelerationGeometryIndexDescriptor indexDescriptor;
        indexDescriptor.offset = 0;
        indexDescriptor.buffer = indexBuffer;
        indexDescriptor.format = WGPUIndexFormat_Uint32;
        indexDescriptor.count = sizeof(indexData) / sizeof(uint32_t);

        WGPURayTracingAccelerationGeometryDescriptor geometry;
        geometry.usage = WGPURayTracingAccelerationGeometryUsage_Opaque;
        geometry.type = WGPURayTracingAccelerationGeometryType_Triangles;
        geometry.vertex = &vertexDescriptor;
        geometry.index = &indexDescriptor;
        geometry.aabb = nullptr;

        WGPURayTracingAccelerationContainerDescriptor descriptor;
        descriptor.level = WGPURayTracingAccelerationContainerLevel_Bottom;
        descriptor.usage = WGPURayTracingAccelerationContainerUsage_PreferFastTrace;
        descriptor.geometryCount = 1;
        descriptor.geometries = &geometry;
        descriptor.instanceCount = 0;
        descriptor.instances = nullptr;

        geometryContainer = wgpuDeviceCreateRayTracingAccelerationContainer(device, &descriptor);
    }

    {
        WGPUTransform3DDescriptor translationDescriptor;
        translationDescriptor.x = 0.0;
        translationDescriptor.y = 0.0;
        translationDescriptor.z = 0.0;
        WGPUTransform3DDescriptor rotationDescriptor;
        rotationDescriptor.x = 0.0;
        rotationDescriptor.y = 0.0;
        rotationDescriptor.z = 0.0;
        WGPUTransform3DDescriptor scaleDescriptor;
        scaleDescriptor.x = 1.0;
        scaleDescriptor.y = 1.0;
        scaleDescriptor.z = 1.0;

        WGPURayTracingAccelerationInstanceTransformDescriptor transformDescriptor;
        transformDescriptor.translation = &translationDescriptor;
        transformDescriptor.rotation = &rotationDescriptor;
        transformDescriptor.scale = &scaleDescriptor;

        WGPURayTracingAccelerationInstanceDescriptor instanceDescriptor;
        instanceDescriptor.usage = WGPURayTracingAccelerationInstanceUsage_TriangleCullDisable;
        instanceDescriptor.instanceId = 0;
        instanceDescriptor.instanceOffset = 0x0;
        instanceDescriptor.mask = 0xFF;
        instanceDescriptor.geometryContainer = geometryContainer;
        instanceDescriptor.transformMatrix = nullptr;
        instanceDescriptor.transformMatrixSize = 0;
        instanceDescriptor.transform = &transformDescriptor;

        WGPURayTracingAccelerationContainerDescriptor descriptor;
        descriptor.level = WGPURayTracingAccelerationContainerLevel_Top;
        descriptor.usage = WGPURayTracingAccelerationContainerUsage_PreferFastTrace;
        descriptor.geometryCount = 0;
        descriptor.geometries = nullptr;
        descriptor.instanceCount = 1;
        descriptor.instances = &instanceDescriptor;

        instanceContainer = wgpuDeviceCreateRayTracingAccelerationContainer(device, &descriptor);
    }

    {
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        wgpuCommandEncoderBuildRayTracingAccelerationContainer(encoder, geometryContainer);
        WGPUCommandBuffer commandBuffer = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuQueueSubmit(queue, 1, &commandBuffer);

        wgpuCommandEncoderRelease(encoder);
        wgpuCommandBufferRelease(commandBuffer);
    }

    {
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        wgpuCommandEncoderBuildRayTracingAccelerationContainer(encoder, instanceContainer);
        WGPUCommandBuffer commandBuffer = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuQueueSubmit(queue, 1, &commandBuffer);

        wgpuCommandEncoderRelease(encoder);
        wgpuCommandBufferRelease(commandBuffer);
    }

    WGPURayTracingShaderBindingTable sbt;
    {
        WGPURayTracingShaderBindingTableStageDescriptor stageDescriptors[3];
        stageDescriptors[0].stage = WGPUShaderStage_RayGeneration;
        stageDescriptors[0].module = rayGenModule;
        stageDescriptors[1].stage = WGPUShaderStage_RayClosestHit;
        stageDescriptors[1].module = rayCHitModule;
        stageDescriptors[2].stage = WGPUShaderStage_RayMiss;
        stageDescriptors[2].module = rayMissModule;

        WGPURayTracingShaderBindingTableGroupDescriptor groupDescriptors[3];
        // gen
        groupDescriptors[0].type = WGPURayTracingShaderBindingTableGroupType_General;
        groupDescriptors[0].generalIndex = 0;
        groupDescriptors[0].closestHitIndex = -1;
        groupDescriptors[0].anyHitIndex = -1;
        groupDescriptors[0].intersectionIndex = -1;
        // hit
        groupDescriptors[1].type = WGPURayTracingShaderBindingTableGroupType_TrianglesHitGroup;
        groupDescriptors[1].generalIndex = -1;
        groupDescriptors[1].closestHitIndex = 1;
        groupDescriptors[1].anyHitIndex = -1;
        groupDescriptors[1].intersectionIndex = -1;
        // miss
        groupDescriptors[2].type = WGPURayTracingShaderBindingTableGroupType_General;
        groupDescriptors[2].generalIndex = 2;
        groupDescriptors[2].closestHitIndex = -1;
        groupDescriptors[2].anyHitIndex = -1;
        groupDescriptors[2].intersectionIndex = -1;

        WGPURayTracingShaderBindingTableDescriptor descriptor{};
        descriptor.stageCount = 3;
        descriptor.stages = stageDescriptors;
        descriptor.groupCount = 3;
        descriptor.groups = groupDescriptors;

        sbt = wgpuDeviceCreateRayTracingShaderBindingTable(device, &descriptor);
    }

    {
        WGPUBindGroupLayoutEntry entryDescriptors[2];
        // acceleration structure
        entryDescriptors[0] = {};
        entryDescriptors[0].binding = 0;
        entryDescriptors[0].type = WGPUBindingType_AccelerationContainer;
        entryDescriptors[0].visibility = WGPUShaderStage_RayGeneration;
        // pixel buffer
        entryDescriptors[1] = {};
        entryDescriptors[1].binding = 1;
        entryDescriptors[1].type = WGPUBindingType_StorageBuffer;
        entryDescriptors[1].visibility = WGPUShaderStage_RayGeneration;

        WGPUBindGroupLayoutDescriptor descriptor;
        descriptor.label = nullptr;
        descriptor.nextInChain = nullptr;
        descriptor.entryCount = 2;
        descriptor.entries = entryDescriptors;

        rtBindGroupLayout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
    }

    {
        WGPUBindGroupEntry entryDescriptors[2];
        // acceleration container
        entryDescriptors[0] = {};
        entryDescriptors[0].binding = 0;
        entryDescriptors[0].offset = 0;
        entryDescriptors[0].size = 0;
        entryDescriptors[0].buffer = nullptr;
        entryDescriptors[0].sampler = nullptr;
        entryDescriptors[0].textureView = nullptr;
        entryDescriptors[0].accelerationContainer = instanceContainer;
        // storage buffer
        entryDescriptors[1] = {};
        entryDescriptors[1].binding = 1;
        entryDescriptors[1].offset = 0;
        entryDescriptors[1].size = pixelBufferSize;
        entryDescriptors[1].buffer = pixelBuffer;
        entryDescriptors[1].sampler = nullptr;
        entryDescriptors[1].textureView = nullptr;
        entryDescriptors[1].accelerationContainer = nullptr;

        WGPUBindGroupDescriptor descriptor;
        descriptor.label = nullptr;
        descriptor.nextInChain = nullptr;
        descriptor.layout = rtBindGroupLayout;
        descriptor.entryCount = 2;
        descriptor.entries = entryDescriptors;

        rtBindGroup = wgpuDeviceCreateBindGroup(device, &descriptor);
    }

    {
        WGPUPipelineLayoutDescriptor descriptor;
        descriptor.label = nullptr;
        descriptor.nextInChain = nullptr;
        descriptor.bindGroupLayoutCount = 1;
        descriptor.bindGroupLayouts = &rtBindGroupLayout;

        rtPipelineLayout = wgpuDeviceCreatePipelineLayout(device, &descriptor);
    }

    {
        WGPURayTracingStateDescriptor rtStateDescriptor;
        rtStateDescriptor.maxRecursionDepth = 1;
        rtStateDescriptor.maxPayloadSize = 3 * sizeof(float);
        rtStateDescriptor.shaderBindingTable = sbt;

        WGPURayTracingPipelineDescriptor descriptor;
        descriptor.label = nullptr;
        descriptor.layout = rtPipelineLayout;
        descriptor.rayTracingState = &rtStateDescriptor;

        rtPipeline = wgpuDeviceCreateRayTracingPipeline(device, &descriptor);
    }

    {
        WGPUBindGroupLayoutEntry entryDescriptors[1];
        // pixel buffer
        entryDescriptors[0] = {};
        entryDescriptors[0].binding = 0;
        entryDescriptors[0].type = WGPUBindingType_StorageBuffer;
        entryDescriptors[0].visibility = WGPUShaderStage_Fragment;

        WGPUBindGroupLayoutDescriptor descriptor;
        descriptor.label = nullptr;
        descriptor.nextInChain = nullptr;
        descriptor.entryCount = 1;
        descriptor.entries = entryDescriptors;

        bindGroupLayout = wgpuDeviceCreateBindGroupLayout(device, &descriptor);
    }

    {
        WGPUBindGroupEntry entryDescriptors[1];
        // storage buffer
        entryDescriptors[0] = {};
        entryDescriptors[0].binding = 0;
        entryDescriptors[0].offset = 0;
        entryDescriptors[0].size = pixelBufferSize;
        entryDescriptors[0].buffer = pixelBuffer;
        entryDescriptors[0].sampler = nullptr;
        entryDescriptors[0].textureView = nullptr;
        entryDescriptors[0].accelerationContainer = nullptr;

        WGPUBindGroupDescriptor descriptor;
        descriptor.label = nullptr;
        descriptor.nextInChain = nullptr;
        descriptor.layout = bindGroupLayout;
        descriptor.entryCount = 1;
        descriptor.entries = entryDescriptors;

        bindGroup = wgpuDeviceCreateBindGroup(device, &descriptor);
    }

    {
        WGPURenderPipelineDescriptor descriptor;
        descriptor.label = nullptr;
        descriptor.nextInChain = nullptr;

        descriptor.vertexStage.nextInChain = nullptr;
        descriptor.vertexStage.module = vsModule;
        descriptor.vertexStage.entryPoint = "main";

        WGPUProgrammableStageDescriptor fragmentStage;
        fragmentStage.nextInChain = nullptr;
        fragmentStage.module = fsModule;
        fragmentStage.entryPoint = "main";
        descriptor.fragmentStage = &fragmentStage;

        descriptor.sampleCount = 1;

        WGPUBlendDescriptor blendDescriptor;
        blendDescriptor.operation = WGPUBlendOperation_Add;
        blendDescriptor.srcFactor = WGPUBlendFactor_One;
        blendDescriptor.dstFactor = WGPUBlendFactor_One;
        WGPUColorStateDescriptor colorStateDescriptor;
        colorStateDescriptor.nextInChain = nullptr;
        colorStateDescriptor.format = swapChainFormat;
        colorStateDescriptor.alphaBlend = blendDescriptor;
        colorStateDescriptor.colorBlend = blendDescriptor;
        colorStateDescriptor.writeMask = WGPUColorWriteMask_All;

        descriptor.colorStateCount = 1;
        descriptor.colorStates = &colorStateDescriptor;

        WGPUPipelineLayoutDescriptor pl;
        pl.nextInChain = nullptr;
        pl.label = nullptr;
        pl.bindGroupLayoutCount = 1;
        pl.bindGroupLayouts = &bindGroupLayout;
        descriptor.layout = wgpuDeviceCreatePipelineLayout(device, &pl);

        WGPUVertexStateDescriptor vertexState;
        vertexState.nextInChain = nullptr;
        vertexState.indexFormat = WGPUIndexFormat_Uint32;
        vertexState.vertexBufferCount = 0;
        vertexState.vertexBuffers = nullptr;
        descriptor.vertexState = &vertexState;

        WGPURasterizationStateDescriptor rasterizationState;
        rasterizationState.nextInChain = nullptr;
        rasterizationState.frontFace = WGPUFrontFace_CCW;
        rasterizationState.cullMode = WGPUCullMode_None;
        rasterizationState.depthBias = 0;
        rasterizationState.depthBiasSlopeScale = 0.0;
        rasterizationState.depthBiasClamp = 0.0;
        descriptor.rasterizationState = &rasterizationState;

        descriptor.primitiveTopology = WGPUPrimitiveTopology_TriangleList;
        descriptor.sampleMask = 0xFFFFFFFF;
        descriptor.alphaToCoverageEnabled = false;

        descriptor.depthStencilState = nullptr;

        pipeline = wgpuDeviceCreateRenderPipeline(device, &descriptor);
    }

    wgpuShaderModuleRelease(vsModule);
    wgpuShaderModuleRelease(fsModule);
}

void frame() {
    WGPUTextureView backbufferView = wgpuSwapChainGetCurrentTextureView(swapchain);

    {
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);

        WGPURayTracingPassDescriptor rayTracingPassInfo;
        rayTracingPassInfo.label = nullptr;
        rayTracingPassInfo.nextInChain = nullptr;

        WGPURayTracingPassEncoder pass =
            wgpuCommandEncoderBeginRayTracingPass(encoder, &rayTracingPassInfo);
        wgpuRayTracingPassEncoderSetPipeline(pass, rtPipeline);
        wgpuRayTracingPassEncoderSetBindGroup(pass, 0, rtBindGroup, 0, nullptr);
        wgpuRayTracingPassEncoderTraceRays(pass, 0, 1, 2, width, height, 1);
        wgpuRayTracingPassEncoderEndPass(pass);
        wgpuRayTracingPassEncoderRelease(pass);

        WGPUCommandBuffer commandBuffer = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuCommandEncoderRelease(encoder);
        wgpuQueueSubmit(queue, 1, &commandBuffer);
        wgpuCommandBufferRelease(commandBuffer);
    }

    {
        WGPURenderPassDescriptor renderpassInfo;
        renderpassInfo.nextInChain = nullptr;
        renderpassInfo.label = nullptr;

        WGPURenderPassColorAttachmentDescriptor colorAttachment;
        {
            colorAttachment.attachment = backbufferView;
            colorAttachment.resolveTarget = nullptr;
            colorAttachment.clearColor = {0.0f, 0.0f, 0.0f, 1.0f};
            colorAttachment.loadOp = WGPULoadOp_Clear;
            colorAttachment.storeOp = WGPUStoreOp_Store;
            renderpassInfo.colorAttachmentCount = 1;
            renderpassInfo.colorAttachments = &colorAttachment;
            renderpassInfo.depthStencilAttachment = nullptr;
        }

        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);

        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &renderpassInfo);
        wgpuRenderPassEncoderSetPipeline(pass, pipeline);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, bindGroup, 0, nullptr);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderEndPass(pass);
        wgpuRenderPassEncoderRelease(pass);

        WGPUCommandBuffer commandBuffer = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuCommandEncoderRelease(encoder);
        wgpuQueueSubmit(queue, 1, &commandBuffer);
        wgpuCommandBufferRelease(commandBuffer);
    }

    wgpuSwapChainPresent(swapchain);
    wgpuTextureViewRelease(backbufferView);

    DoFlush();
}

int main(int argc, const char* argv[]) {
    if (!InitSample(argc, argv)) {
        return 1;
    }
    init();

    while (!ShouldQuit()) {
        frame();
        utils::USleep(16000);
    }

    // TODO release stuff
}