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

#include "dawn_native/d3d12/CommandBufferD3D12.h"

#include <deque>

#include "common/Assert.h"
#include "dawn_native/BindGroupAndStorageBarrierTracker.h"
#include "dawn_native/CommandEncoder.h"
#include "dawn_native/Commands.h"
#include "dawn_native/RenderBundle.h"
#include "dawn_native/d3d12/BindGroupD3D12.h"
#include "dawn_native/d3d12/BindGroupLayoutD3D12.h"
#include "dawn_native/d3d12/BufferD3D12.h"
#include "dawn_native/d3d12/CommandRecordingContext.h"
#include "dawn_native/d3d12/ComputePipelineD3D12.h"
#include "dawn_native/d3d12/DeviceD3D12.h"
#include "dawn_native/d3d12/PipelineLayoutD3D12.h"
#include "dawn_native/d3d12/PlatformFunctions.h"
#include "dawn_native/d3d12/RayTracingAccelerationContainerD3D12.h"
#include "dawn_native/d3d12/RayTracingPipelineD3D12.h"
#include "dawn_native/d3d12/RayTracingShaderBindingTableD3D12.h"
#include "dawn_native/d3d12/RenderPassBuilderD3D12.h"
#include "dawn_native/d3d12/RenderPipelineD3D12.h"
#include "dawn_native/d3d12/SamplerD3D12.h"
#include "dawn_native/d3d12/SamplerHeapCacheD3D12.h"
#include "dawn_native/d3d12/ShaderVisibleDescriptorAllocatorD3D12.h"
#include "dawn_native/d3d12/StagingDescriptorAllocatorD3D12.h"
#include "dawn_native/d3d12/TextureCopySplitter.h"
#include "dawn_native/d3d12/TextureD3D12.h"
#include "dawn_native/d3d12/UtilsD3D12.h"

namespace dawn_native { namespace d3d12 {

    namespace {

        DXGI_FORMAT DXGIIndexFormat(wgpu::IndexFormat format) {
            switch (format) {
                case wgpu::IndexFormat::Uint16:
                    return DXGI_FORMAT_R16_UINT;
                case wgpu::IndexFormat::Uint32:
                    return DXGI_FORMAT_R32_UINT;
                default:
                    UNREACHABLE();
            }
        }

        bool CanUseCopyResource(const Texture* src, const Texture* dst, const Extent3D& copySize) {
            // Checked by validation
            ASSERT(src->GetSampleCount() == dst->GetSampleCount());
            ASSERT(src->GetFormat().format == dst->GetFormat().format);

            const Extent3D& srcSize = src->GetSize();
            const Extent3D& dstSize = dst->GetSize();

            auto GetCopyDepth = [](const Texture* texture) {
                switch (texture->GetDimension()) {
                    case wgpu::TextureDimension::e1D:
                        return 1u;
                    case wgpu::TextureDimension::e2D:
                        return texture->GetArrayLayers();
                    case wgpu::TextureDimension::e3D:
                        return texture->GetSize().depth;
                }
            };

            // https://docs.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist-copyresource
            // In order to use D3D12's copy resource, the textures must be the same dimensions, and
            // the copy must be of the entire resource.
            // TODO(dawn:129): Support 1D textures.
            return src->GetDimension() == dst->GetDimension() &&  //
                   dst->GetNumMipLevels() == 1 &&                 //
                   src->GetNumMipLevels() == 1 &&      // A copy command is of a single mip, so if a
                                                       // resource has more than one, we definitely
                                                       // cannot use CopyResource.
                   copySize.width == dstSize.width &&  //
                   copySize.width == srcSize.width &&  //
                   copySize.height == dstSize.height &&    //
                   copySize.height == srcSize.height &&    //
                   copySize.depth == GetCopyDepth(src) &&  //
                   copySize.depth == GetCopyDepth(dst);
        }

    }  // anonymous namespace

    class BindGroupStateTracker : public BindGroupAndStorageBarrierTrackerBase<false, uint64_t> {
      public:
        BindGroupStateTracker(Device* device)
            : BindGroupAndStorageBarrierTrackerBase(),
              mDevice(device),
              mViewAllocator(device->GetViewShaderVisibleDescriptorAllocator()),
              mSamplerAllocator(device->GetSamplerShaderVisibleDescriptorAllocator()) {
        }

        void SetInComputePass(bool inCompute_) {
            mInCompute = inCompute_;
        }

        void SetInRayTracingPass(bool inRayTracing_) {
            mInRayTracing = inRayTracing_;
        }

        MaybeError Apply(CommandRecordingContext* commandContext) {
            // Bindgroups are allocated in shader-visible descriptor heaps which are managed by a
            // ringbuffer. There can be a single shader-visible descriptor heap of each type bound
            // at any given time. This means that when we switch heaps, all other currently bound
            // bindgroups must be re-populated. Bindgroups can fail allocation gracefully which is
            // the signal to change the bounded heaps.
            // Re-populating all bindgroups after the last one fails causes duplicated allocations
            // to occur on overflow.
            // TODO(bryan.bernhart@intel.com): Consider further optimization.
            bool didCreateBindGroupViews = true;
            bool didCreateBindGroupSamplers = true;
            for (uint32_t index : IterateBitSet(mDirtyBindGroups)) {
                BindGroup* group = ToBackend(mBindGroups[index]);
                didCreateBindGroupViews = group->PopulateViews(mViewAllocator);
                didCreateBindGroupSamplers = group->PopulateSamplers(mDevice, mSamplerAllocator);
                if (!didCreateBindGroupViews && !didCreateBindGroupSamplers) {
                    break;
                }
            }

            ID3D12GraphicsCommandList* commandList = commandContext->GetCommandList();

            if (!didCreateBindGroupViews || !didCreateBindGroupSamplers) {
                if (!didCreateBindGroupViews) {
                    DAWN_TRY(mViewAllocator->AllocateAndSwitchShaderVisibleHeap());
                }

                if (!didCreateBindGroupSamplers) {
                    DAWN_TRY(mSamplerAllocator->AllocateAndSwitchShaderVisibleHeap());
                }

                mDirtyBindGroupsObjectChangedOrIsDynamic |= mBindGroupLayoutsMask;
                mDirtyBindGroups |= mBindGroupLayoutsMask;

                // Must be called before applying the bindgroups.
                SetID3D12DescriptorHeaps(commandList);

                for (uint32_t index : IterateBitSet(mBindGroupLayoutsMask)) {
                    BindGroup* group = ToBackend(mBindGroups[index]);
                    didCreateBindGroupViews = group->PopulateViews(mViewAllocator);
                    didCreateBindGroupSamplers =
                        group->PopulateSamplers(mDevice, mSamplerAllocator);
                    ASSERT(didCreateBindGroupViews);
                    ASSERT(didCreateBindGroupSamplers);
                }
            }

            for (uint32_t index : IterateBitSet(mDirtyBindGroupsObjectChangedOrIsDynamic)) {
                BindGroup* group = ToBackend(mBindGroups[index]);
                ApplyBindGroup(commandList, ToBackend(mPipelineLayout), index, group,
                               mDynamicOffsetCounts[index], mDynamicOffsets[index].data());
            }

            if (mInCompute || mInRayTracing) {
                for (uint32_t index : IterateBitSet(mBindGroupLayoutsMask)) {
                    for (uint32_t binding : IterateBitSet(mBindingsNeedingBarrier[index])) {
                        wgpu::BindingType bindingType = mBindingTypes[index][binding];
                        switch (bindingType) {
                            case wgpu::BindingType::StorageBuffer:
                                static_cast<Buffer*>(mBindings[index][binding])
                                    ->TrackUsageAndTransitionNow(commandContext,
                                                                 wgpu::BufferUsage::Storage);
                                break;

                            case wgpu::BindingType::ReadonlyStorageTexture:
                                ToBackend(static_cast<TextureView*>(mBindings[index][binding])
                                              ->GetTexture())
                                    ->TrackUsageAndTransitionNow(commandContext,
                                                                 kReadonlyStorageTexture);
                                break;

                            case wgpu::BindingType::WriteonlyStorageTexture:
                                ToBackend(static_cast<TextureView*>(mBindings[index][binding])
                                              ->GetTexture())
                                    ->TrackUsageAndTransitionNow(commandContext,
                                                                 wgpu::TextureUsage::Storage);
                                break;

                            case wgpu::BindingType::StorageTexture:
                                // Not implemented.

                            case wgpu::BindingType::UniformBuffer:
                            case wgpu::BindingType::ReadonlyStorageBuffer:
                            case wgpu::BindingType::Sampler:
                            case wgpu::BindingType::ComparisonSampler:
                            case wgpu::BindingType::SampledTexture:
                                // Don't require barriers.

                            default:
                                UNREACHABLE();
                                break;
                        }
                    }
                }
            }
            DidApply();

            return {};
        }

        void SetID3D12DescriptorHeaps(ID3D12GraphicsCommandList* commandList) {
            ASSERT(commandList != nullptr);
            std::array<ID3D12DescriptorHeap*, 2> descriptorHeaps = {
                mViewAllocator->GetShaderVisibleHeap(), mSamplerAllocator->GetShaderVisibleHeap()};
            ASSERT(descriptorHeaps[0] != nullptr);
            ASSERT(descriptorHeaps[1] != nullptr);
            commandList->SetDescriptorHeaps(descriptorHeaps.size(), descriptorHeaps.data());
        }

      private:
        void ApplyBindGroup(ID3D12GraphicsCommandList* commandList,
                            const PipelineLayout* pipelineLayout,
                            uint32_t index,
                            BindGroup* group,
                            uint32_t dynamicOffsetCount,
                            const uint64_t* dynamicOffsets) {
            ASSERT(dynamicOffsetCount == group->GetLayout()->GetDynamicBufferCount());

            // Usually, the application won't set the same offsets many times,
            // so always try to apply dynamic offsets even if the offsets stay the same
            if (dynamicOffsetCount != 0) {
                // Update dynamic offsets.
                // Dynamic buffer bindings are packed at the beginning of the layout.
                for (BindingIndex bindingIndex = 0; bindingIndex < dynamicOffsetCount;
                     ++bindingIndex) {
                    uint32_t parameterIndex =
                        pipelineLayout->GetDynamicRootParameterIndex(index, bindingIndex);
                    BufferBinding binding = group->GetBindingAsBufferBinding(bindingIndex);

                    // Calculate buffer locations that root descriptors links to. The location
                    // is (base buffer location + initial offset + dynamic offset)
                    uint64_t dynamicOffset = dynamicOffsets[bindingIndex];
                    uint64_t offset = binding.offset + dynamicOffset;
                    D3D12_GPU_VIRTUAL_ADDRESS bufferLocation =
                        ToBackend(binding.buffer)->GetVA() + offset;

                    switch (group->GetLayout()->GetBindingInfo(bindingIndex).type) {
                        case wgpu::BindingType::UniformBuffer:
                            if (mInCompute || mInRayTracing) {
                                commandList->SetComputeRootConstantBufferView(parameterIndex,
                                                                              bufferLocation);
                            } else {
                                commandList->SetGraphicsRootConstantBufferView(parameterIndex,
                                                                               bufferLocation);
                            }
                            break;
                        case wgpu::BindingType::StorageBuffer:
                            if (mInCompute || mInRayTracing) {
                                commandList->SetComputeRootUnorderedAccessView(parameterIndex,
                                                                               bufferLocation);
                            } else {
                                commandList->SetGraphicsRootUnorderedAccessView(parameterIndex,
                                                                                bufferLocation);
                            }
                            break;
                        case wgpu::BindingType::ReadonlyStorageBuffer:
                            if (mInCompute || mInRayTracing) {
                                commandList->SetComputeRootShaderResourceView(parameterIndex,
                                                                              bufferLocation);
                            } else {
                                commandList->SetGraphicsRootShaderResourceView(parameterIndex,
                                                                               bufferLocation);
                            }
                            break;
                        case wgpu::BindingType::SampledTexture:
                        case wgpu::BindingType::Sampler:
                        case wgpu::BindingType::ComparisonSampler:
                        case wgpu::BindingType::StorageTexture:
                        case wgpu::BindingType::ReadonlyStorageTexture:
                        case wgpu::BindingType::WriteonlyStorageTexture:
                        case wgpu::BindingType::AccelerationContainer:
                            UNREACHABLE();
                            break;
                    }
                }
            }

            // It's not necessary to update descriptor tables if only the dynamic offset changed.
            if (!mDirtyBindGroups[index]) {
                return;
            }

            const uint32_t cbvUavSrvCount =
                ToBackend(group->GetLayout())->GetCbvUavSrvDescriptorCount();
            const uint32_t samplerCount =
                ToBackend(group->GetLayout())->GetSamplerDescriptorCount();

            if (cbvUavSrvCount > 0) {
                uint32_t parameterIndex = pipelineLayout->GetCbvUavSrvRootParameterIndex(index);
                const D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor = group->GetBaseViewDescriptor();
                if (mInCompute || mInRayTracing) {
                    commandList->SetComputeRootDescriptorTable(parameterIndex, baseDescriptor);
                } else {
                    commandList->SetGraphicsRootDescriptorTable(parameterIndex, baseDescriptor);
                }
            }

            if (samplerCount > 0) {
                uint32_t parameterIndex = pipelineLayout->GetSamplerRootParameterIndex(index);
                const D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor =
                    group->GetBaseSamplerDescriptor();
                if (mInCompute || mInRayTracing) {
                    commandList->SetComputeRootDescriptorTable(parameterIndex, baseDescriptor);
                } else {
                    commandList->SetGraphicsRootDescriptorTable(parameterIndex, baseDescriptor);
                }
            }
        }

        Device* mDevice;

        bool mInCompute = false;
        bool mInRayTracing = false;

        ShaderVisibleDescriptorAllocator* mViewAllocator;
        ShaderVisibleDescriptorAllocator* mSamplerAllocator;
    };

    namespace {
        class VertexBufferTracker {
          public:
            void OnSetVertexBuffer(uint32_t slot, Buffer* buffer, uint64_t offset, uint64_t size) {
                mStartSlot = std::min(mStartSlot, slot);
                mEndSlot = std::max(mEndSlot, slot + 1);

                auto* d3d12BufferView = &mD3D12BufferViews[slot];
                d3d12BufferView->BufferLocation = buffer->GetVA() + offset;
                d3d12BufferView->SizeInBytes = size;
                // The bufferView stride is set based on the vertex state before a draw.
            }

            void Apply(ID3D12GraphicsCommandList* commandList,
                       const RenderPipeline* renderPipeline) {
                ASSERT(renderPipeline != nullptr);

                std::bitset<kMaxVertexBuffers> vertexBufferSlotsUsed =
                    renderPipeline->GetVertexBufferSlotsUsed();

                uint32_t startSlot = mStartSlot;
                uint32_t endSlot = mEndSlot;

                // If the vertex state has changed, we need to update the StrideInBytes
                // for the D3D12 buffer views. We also need to extend the dirty range to
                // touch all these slots because the stride may have changed.
                if (mLastAppliedRenderPipeline != renderPipeline) {
                    mLastAppliedRenderPipeline = renderPipeline;

                    for (uint32_t slot : IterateBitSet(vertexBufferSlotsUsed)) {
                        startSlot = std::min(startSlot, slot);
                        endSlot = std::max(endSlot, slot + 1);
                        mD3D12BufferViews[slot].StrideInBytes =
                            renderPipeline->GetVertexBuffer(slot).arrayStride;
                    }
                }

                if (endSlot <= startSlot) {
                    return;
                }

                // mD3D12BufferViews is kept up to date with the most recent data passed
                // to SetVertexBuffer. This makes it correct to only track the start
                // and end of the dirty range. When Apply is called,
                // we will at worst set non-dirty vertex buffers in duplicate.
                uint32_t count = endSlot - startSlot;
                commandList->IASetVertexBuffers(startSlot, count, &mD3D12BufferViews[startSlot]);

                mStartSlot = kMaxVertexBuffers;
                mEndSlot = 0;
            }

          private:
            // startSlot and endSlot indicate the range of dirty vertex buffers.
            // If there are multiple calls to SetVertexBuffer, the start and end
            // represent the union of the dirty ranges (the union may have non-dirty
            // data in the middle of the range).
            const RenderPipeline* mLastAppliedRenderPipeline = nullptr;
            uint32_t mStartSlot = kMaxVertexBuffers;
            uint32_t mEndSlot = 0;
            std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVertexBuffers> mD3D12BufferViews = {};
        };

        class IndexBufferTracker {
          public:
            void OnSetIndexBuffer(Buffer* buffer, uint64_t offset, uint64_t size) {
                mD3D12BufferView.BufferLocation = buffer->GetVA() + offset;
                mD3D12BufferView.SizeInBytes = size;

                // We don't need to dirty the state unless BufferLocation or SizeInBytes
                // change, but most of the time this will always be the case.
                mLastAppliedIndexFormat = DXGI_FORMAT_UNKNOWN;
            }

            void OnSetPipeline(const RenderPipelineBase* pipeline) {
                mD3D12BufferView.Format =
                    DXGIIndexFormat(pipeline->GetVertexStateDescriptor()->indexFormat);
            }

            void Apply(ID3D12GraphicsCommandList* commandList) {
                if (mD3D12BufferView.Format == mLastAppliedIndexFormat) {
                    return;
                }

                commandList->IASetIndexBuffer(&mD3D12BufferView);
                mLastAppliedIndexFormat = mD3D12BufferView.Format;
            }

          private:
            DXGI_FORMAT mLastAppliedIndexFormat = DXGI_FORMAT_UNKNOWN;
            D3D12_INDEX_BUFFER_VIEW mD3D12BufferView = {};
        };

        void ResolveMultisampledRenderPass(CommandRecordingContext* commandContext,
                                           BeginRenderPassCmd* renderPass) {
            ASSERT(renderPass != nullptr);

            for (uint32_t i :
                 IterateBitSet(renderPass->attachmentState->GetColorAttachmentsMask())) {
                TextureViewBase* resolveTarget =
                    renderPass->colorAttachments[i].resolveTarget.Get();
                if (resolveTarget == nullptr) {
                    continue;
                }

                Texture* colorTexture =
                    ToBackend(renderPass->colorAttachments[i].view->GetTexture());
                Texture* resolveTexture = ToBackend(resolveTarget->GetTexture());

                // Transition the usages of the color attachment and resolve target.
                colorTexture->TrackUsageAndTransitionNow(commandContext,
                                                         D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                resolveTexture->TrackUsageAndTransitionNow(commandContext,
                                                           D3D12_RESOURCE_STATE_RESOLVE_DEST);

                // Do MSAA resolve with ResolveSubResource().
                ID3D12Resource* colorTextureHandle = colorTexture->GetD3D12Resource();
                ID3D12Resource* resolveTextureHandle = resolveTexture->GetD3D12Resource();
                const uint32_t resolveTextureSubresourceIndex = resolveTexture->GetSubresourceIndex(
                    resolveTarget->GetBaseMipLevel(), resolveTarget->GetBaseArrayLayer());
                constexpr uint32_t kColorTextureSubresourceIndex = 0;
                commandContext->GetCommandList()->ResolveSubresource(
                    resolveTextureHandle, resolveTextureSubresourceIndex, colorTextureHandle,
                    kColorTextureSubresourceIndex, colorTexture->GetD3D12Format());
            }
        }

    }  // anonymous namespace

    CommandBuffer::CommandBuffer(CommandEncoder* encoder, const CommandBufferDescriptor* descriptor)
        : CommandBufferBase(encoder, descriptor), mCommands(encoder->AcquireCommands()) {
    }

    CommandBuffer::~CommandBuffer() {
        FreeCommands(&mCommands);
    }

    MaybeError CommandBuffer::RecordCommands(CommandRecordingContext* commandContext) {
        Device* device = ToBackend(GetDevice());
        BindGroupStateTracker bindingTracker(device);

        ID3D12GraphicsCommandList* commandList = commandContext->GetCommandList();
        ID3D12GraphicsCommandList4* commandList4 = commandContext->GetCommandList4();

        // Make sure we use the correct descriptors for this command list. Could be done once per
        // actual command list but here is ok because there should be few command buffers.
        bindingTracker.SetID3D12DescriptorHeaps(commandList);

        // Records the necessary barriers for the resource usage pre-computed by the frontend
        auto PrepareResourcesForSubmission = [](CommandRecordingContext* commandContext,
                                                const PassResourceUsage& usages) -> bool {
            std::vector<D3D12_RESOURCE_BARRIER> barriers;

            ID3D12GraphicsCommandList* commandList = commandContext->GetCommandList();

            wgpu::BufferUsage bufferUsages = wgpu::BufferUsage::None;

            for (size_t i = 0; i < usages.buffers.size(); ++i) {
                D3D12_RESOURCE_BARRIER barrier;
                if (ToBackend(usages.buffers[i])
                        ->TrackUsageAndGetResourceBarrier(commandContext, &barrier,
                                                          usages.bufferUsages[i])) {
                    barriers.push_back(barrier);
                }
                bufferUsages |= usages.bufferUsages[i];
            }

            for (size_t i = 0; i < usages.textures.size(); ++i) {
                Texture* texture = ToBackend(usages.textures[i]);
                // Clear textures that are not output attachments. Output attachments will be
                // cleared during record render pass if the texture subresource has not been
                // initialized before the render pass.
                if (!(usages.textureUsages[i].usage & wgpu::TextureUsage::OutputAttachment)) {
                    texture->EnsureSubresourceContentInitialized(commandContext, 0,
                                                                 texture->GetNumMipLevels(), 0,
                                                                 texture->GetArrayLayers());
                }
            }

            wgpu::TextureUsage textureUsages = wgpu::TextureUsage::None;

            for (size_t i = 0; i < usages.textures.size(); ++i) {
                D3D12_RESOURCE_BARRIER barrier;
                if (ToBackend(usages.textures[i])
                        ->TrackUsageAndGetResourceBarrier(commandContext, &barrier,
                                                          usages.textureUsages[i].usage)) {
                    barriers.push_back(barrier);
                }
                textureUsages |= usages.textureUsages[i].usage;
            }

            if (barriers.size()) {
                commandList->ResourceBarrier(barriers.size(), barriers.data());
            }

            return (bufferUsages & wgpu::BufferUsage::Storage ||
                    textureUsages & wgpu::TextureUsage::Storage);
        };

        const std::vector<PassResourceUsage>& passResourceUsages = GetResourceUsages().perPass;
        uint32_t nextPassNumber = 0;

        RayTracingAccelerationContainer* lastBuildContainer = nullptr;
        RayTracingAccelerationContainer* lastUpdateContainer = nullptr;

        Command type;
        while (mCommands.NextCommandId(&type)) {
            switch (type) {
                case Command::BeginComputePass: {
                    mCommands.NextCommand<BeginComputePassCmd>();

                    PrepareResourcesForSubmission(commandContext,
                                                  passResourceUsages[nextPassNumber]);
                    bindingTracker.SetInComputePass(true);
                    DAWN_TRY(RecordComputePass(commandContext, &bindingTracker));

                    nextPassNumber++;
                    break;
                }

                case Command::BeginRenderPass: {
                    BeginRenderPassCmd* beginRenderPassCmd =
                        mCommands.NextCommand<BeginRenderPassCmd>();

                    const bool passHasUAV = PrepareResourcesForSubmission(
                        commandContext, passResourceUsages[nextPassNumber]);
                    bindingTracker.SetInComputePass(false);

                    LazyClearRenderPassAttachments(beginRenderPassCmd);
                    DAWN_TRY(RecordRenderPass(commandContext, &bindingTracker, beginRenderPassCmd,
                                              passHasUAV));

                    nextPassNumber++;
                    break;
                }

                case Command::BeginRayTracingPass: {
                    mCommands.NextCommand<BeginRayTracingPassCmd>();

                    PrepareResourcesForSubmission(commandContext,
                                                  passResourceUsages[nextPassNumber]);
                    bindingTracker.SetInRayTracingPass(true);
                    DAWN_TRY(RecordRayTracingPass(commandContext, &bindingTracker));

                    nextPassNumber++;
                    break;
                }

                case Command::BuildRayTracingAccelerationContainer: {
                    BuildRayTracingAccelerationContainerCmd* build =
                        mCommands.NextCommand<BuildRayTracingAccelerationContainerCmd>();
                    RayTracingAccelerationContainer* container = ToBackend(build->container.Get());

                    MemoryEntry* resultMemory = &container->GetScratchMemory().result;
                    MemoryEntry* buildMemory = &container->GetScratchMemory().build;

                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
                    buildDesc.Inputs = container->GetBuildInformation();
                    buildDesc.SourceAccelerationStructureData = 0;
                    buildDesc.DestAccelerationStructureData = resultMemory->address;
                    buildDesc.ScratchAccelerationStructureData = buildMemory->address;

                    commandList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

                    // barrier for result memory
                    D3D12_RESOURCE_BARRIER uavBarrier;
                    uavBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    uavBarrier.UAV.pResource = resultMemory->buffer.Get();
                    commandList->ResourceBarrier(1, &uavBarrier);

                    container->SetBuildState(true);

                    if (lastUpdateContainer != nullptr) {
                        return DAWN_VALIDATION_ERROR(
                            "Build and update passes for acceleration containers must be "
                            "separated");
                    }
                    if (lastBuildContainer != nullptr &&
                        lastBuildContainer->GetLevel() != container->GetLevel()) {
                        return DAWN_VALIDATION_ERROR(
                            "Acceleration containers of different levels must be built in "
                            "separate passes");
                    }
                    lastBuildContainer = container;
                    break;
                }

                case Command::CopyRayTracingAccelerationContainer: {
                    CopyRayTracingAccelerationContainerCmd* copy =
                        mCommands.NextCommand<CopyRayTracingAccelerationContainerCmd>();
                    RayTracingAccelerationContainer* srcContainer =
                        ToBackend(copy->srcContainer.Get());
                    RayTracingAccelerationContainer* dstContainer =
                        ToBackend(copy->dstContainer.Get());

                    MemoryEntry* srcMemory = &srcContainer->GetScratchMemory().result;
                    MemoryEntry* dstMemory = &dstContainer->GetScratchMemory().result;

                    commandList4->CopyRaytracingAccelerationStructure(
                        dstMemory->address, srcMemory->address,
                        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
                    break;
                }

                case Command::UpdateRayTracingAccelerationContainer: {
                    UpdateRayTracingAccelerationContainerCmd* update =
                        mCommands.NextCommand<UpdateRayTracingAccelerationContainerCmd>();
                    RayTracingAccelerationContainer* container = ToBackend(update->container.Get());

                    // we can destroy the scratch build memory after the first update
                    if (container->IsBuilt() && !container->IsUpdated()) {
                        container->DestroyScratchBuildMemory();
                        container->SetUpdateState(true);
                    }

                    MemoryEntry* resultMemory = &container->GetScratchMemory().result;
                    MemoryEntry* updateMemory = &container->GetScratchMemory().update;

                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc;
                    buildDesc.Inputs = container->GetBuildInformation();
                    buildDesc.SourceAccelerationStructureData = resultMemory->address;
                    buildDesc.DestAccelerationStructureData = resultMemory->address;
                    buildDesc.ScratchAccelerationStructureData = updateMemory->address;

                    buildDesc.Inputs.Flags |=
                        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;

                    commandList4->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);

                    // barrier for result memory
                    D3D12_RESOURCE_BARRIER uavBarrier;
                    uavBarrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    uavBarrier.UAV.pResource = resultMemory->buffer.Get();
                    commandList->ResourceBarrier(1, &uavBarrier);

                    container->SetBuildState(true);

                    if (lastBuildContainer != nullptr) {
                        return DAWN_VALIDATION_ERROR(
                            "Build and update passes for acceleration containers must be "
                            "separated");
                    }
                    if (lastUpdateContainer != nullptr &&
                        lastUpdateContainer->GetLevel() != container->GetLevel()) {
                        return DAWN_VALIDATION_ERROR(
                            "Acceleration containers of different levels must be updated in "
                            "separate passes");
                    }
                    lastUpdateContainer = container;
                    break;
                }

                case Command::CopyBufferToBuffer: {
                    CopyBufferToBufferCmd* copy = mCommands.NextCommand<CopyBufferToBufferCmd>();
                    Buffer* srcBuffer = ToBackend(copy->source.Get());
                    Buffer* dstBuffer = ToBackend(copy->destination.Get());

                    srcBuffer->TrackUsageAndTransitionNow(commandContext,
                                                          wgpu::BufferUsage::CopySrc);
                    dstBuffer->TrackUsageAndTransitionNow(commandContext,
                                                          wgpu::BufferUsage::CopyDst);

                    commandList->CopyBufferRegion(
                        dstBuffer->GetD3D12Resource().Get(), copy->destinationOffset,
                        srcBuffer->GetD3D12Resource().Get(), copy->sourceOffset, copy->size);
                    break;
                }

                case Command::CopyBufferToTexture: {
                    CopyBufferToTextureCmd* copy = mCommands.NextCommand<CopyBufferToTextureCmd>();
                    Buffer* buffer = ToBackend(copy->source.buffer.Get());
                    Texture* texture = ToBackend(copy->destination.texture.Get());

                    if (IsCompleteSubresourceCopiedTo(texture, copy->copySize,
                                                      copy->destination.mipLevel)) {
                        texture->SetIsSubresourceContentInitialized(
                            true, copy->destination.mipLevel, 1, copy->destination.arrayLayer, 1);
                    } else {
                        texture->EnsureSubresourceContentInitialized(
                            commandContext, copy->destination.mipLevel, 1,
                            copy->destination.arrayLayer, 1);
                    }

                    buffer->TrackUsageAndTransitionNow(commandContext, wgpu::BufferUsage::CopySrc);
                    texture->TrackUsageAndTransitionNow(commandContext,
                                                        wgpu::TextureUsage::CopyDst);

                    auto copySplit = ComputeTextureCopySplit(
                        copy->destination.origin, copy->copySize, texture->GetFormat(),
                        copy->source.offset, copy->source.bytesPerRow, copy->source.rowsPerImage);

                    D3D12_TEXTURE_COPY_LOCATION textureLocation =
                        ComputeTextureCopyLocationForTexture(texture, copy->destination.mipLevel,
                                                             copy->destination.arrayLayer);

                    for (uint32_t i = 0; i < copySplit.count; ++i) {
                        TextureCopySplit::CopyInfo& info = copySplit.copies[i];

                        D3D12_TEXTURE_COPY_LOCATION bufferLocation =
                            ComputeBufferLocationForCopyTextureRegion(
                                texture, buffer->GetD3D12Resource().Get(), info.bufferSize,
                                copySplit.offset, copy->source.bytesPerRow);
                        D3D12_BOX sourceRegion =
                            ComputeD3D12BoxFromOffsetAndSize(info.bufferOffset, info.copySize);

                        commandList->CopyTextureRegion(&textureLocation, info.textureOffset.x,
                                                       info.textureOffset.y, info.textureOffset.z,
                                                       &bufferLocation, &sourceRegion);
                    }
                    break;
                }

                case Command::CopyTextureToBuffer: {
                    CopyTextureToBufferCmd* copy = mCommands.NextCommand<CopyTextureToBufferCmd>();
                    Texture* texture = ToBackend(copy->source.texture.Get());
                    Buffer* buffer = ToBackend(copy->destination.buffer.Get());

                    texture->EnsureSubresourceContentInitialized(
                        commandContext, copy->source.mipLevel, 1, copy->source.arrayLayer, 1);

                    texture->TrackUsageAndTransitionNow(commandContext,
                                                        wgpu::TextureUsage::CopySrc);
                    buffer->TrackUsageAndTransitionNow(commandContext, wgpu::BufferUsage::CopyDst);

                    TextureCopySplit copySplit = ComputeTextureCopySplit(
                        copy->source.origin, copy->copySize, texture->GetFormat(),
                        copy->destination.offset, copy->destination.bytesPerRow,
                        copy->destination.rowsPerImage);

                    D3D12_TEXTURE_COPY_LOCATION textureLocation =
                        ComputeTextureCopyLocationForTexture(texture, copy->source.mipLevel,
                                                             copy->source.arrayLayer);

                    for (uint32_t i = 0; i < copySplit.count; ++i) {
                        TextureCopySplit::CopyInfo& info = copySplit.copies[i];

                        D3D12_TEXTURE_COPY_LOCATION bufferLocation =
                            ComputeBufferLocationForCopyTextureRegion(
                                texture, buffer->GetD3D12Resource().Get(), info.bufferSize,
                                copySplit.offset, copy->destination.bytesPerRow);

                        D3D12_BOX sourceRegion =
                            ComputeD3D12BoxFromOffsetAndSize(info.textureOffset, info.copySize);

                        commandList->CopyTextureRegion(&bufferLocation, info.bufferOffset.x,
                                                       info.bufferOffset.y, info.bufferOffset.z,
                                                       &textureLocation, &sourceRegion);
                    }
                    break;
                }

                case Command::CopyTextureToTexture: {
                    CopyTextureToTextureCmd* copy =
                        mCommands.NextCommand<CopyTextureToTextureCmd>();

                    Texture* source = ToBackend(copy->source.texture.Get());
                    Texture* destination = ToBackend(copy->destination.texture.Get());

                    source->EnsureSubresourceContentInitialized(
                        commandContext, copy->source.mipLevel, 1, copy->source.arrayLayer, 1);
                    if (IsCompleteSubresourceCopiedTo(destination, copy->copySize,
                                                      copy->destination.mipLevel)) {
                        destination->SetIsSubresourceContentInitialized(
                            true, copy->destination.mipLevel, 1, copy->destination.arrayLayer, 1);
                    } else {
                        destination->EnsureSubresourceContentInitialized(
                            commandContext, copy->destination.mipLevel, 1,
                            copy->destination.arrayLayer, 1);
                    }
                    source->TrackUsageAndTransitionNow(commandContext, wgpu::TextureUsage::CopySrc);
                    destination->TrackUsageAndTransitionNow(commandContext,
                                                            wgpu::TextureUsage::CopyDst);

                    if (CanUseCopyResource(source, destination, copy->copySize)) {
                        commandList->CopyResource(destination->GetD3D12Resource(),
                                                  source->GetD3D12Resource());
                    } else {
                        D3D12_TEXTURE_COPY_LOCATION srcLocation =
                            ComputeTextureCopyLocationForTexture(source, copy->source.mipLevel,
                                                                 copy->source.arrayLayer);

                        D3D12_TEXTURE_COPY_LOCATION dstLocation =
                            ComputeTextureCopyLocationForTexture(destination,
                                                                 copy->destination.mipLevel,
                                                                 copy->destination.arrayLayer);

                        D3D12_BOX sourceRegion =
                            ComputeD3D12BoxFromOffsetAndSize(copy->source.origin, copy->copySize);

                        commandList->CopyTextureRegion(
                            &dstLocation, copy->destination.origin.x, copy->destination.origin.y,
                            copy->destination.origin.z, &srcLocation, &sourceRegion);
                    }
                    break;
                }

                default: {
                    UNREACHABLE();
                    break;
                }
            }
        }

        return {};
    }

    MaybeError CommandBuffer::RecordComputePass(CommandRecordingContext* commandContext,
                                                BindGroupStateTracker* bindingTracker) {
        PipelineLayout* lastLayout = nullptr;
        ID3D12GraphicsCommandList* commandList = commandContext->GetCommandList();

        Command type;
        while (mCommands.NextCommandId(&type)) {
            switch (type) {
                case Command::Dispatch: {
                    DispatchCmd* dispatch = mCommands.NextCommand<DispatchCmd>();

                    DAWN_TRY(bindingTracker->Apply(commandContext));
                    commandList->Dispatch(dispatch->x, dispatch->y, dispatch->z);
                    break;
                }

                case Command::DispatchIndirect: {
                    DispatchIndirectCmd* dispatch = mCommands.NextCommand<DispatchIndirectCmd>();

                    DAWN_TRY(bindingTracker->Apply(commandContext));
                    Buffer* buffer = ToBackend(dispatch->indirectBuffer.Get());
                    ComPtr<ID3D12CommandSignature> signature =
                        ToBackend(GetDevice())->GetDispatchIndirectSignature();
                    commandList->ExecuteIndirect(signature.Get(), 1,
                                                 buffer->GetD3D12Resource().Get(),
                                                 dispatch->indirectOffset, nullptr, 0);
                    break;
                }

                case Command::EndComputePass: {
                    mCommands.NextCommand<EndComputePassCmd>();
                    return {};
                }

                case Command::SetComputePipeline: {
                    SetComputePipelineCmd* cmd = mCommands.NextCommand<SetComputePipelineCmd>();
                    ComputePipeline* pipeline = ToBackend(cmd->pipeline).Get();
                    PipelineLayout* layout = ToBackend(pipeline->GetLayout());

                    commandList->SetComputeRootSignature(layout->GetRootSignature());
                    commandList->SetPipelineState(pipeline->GetPipelineState());

                    bindingTracker->OnSetPipeline(pipeline);

                    lastLayout = layout;
                    break;
                }

                case Command::SetBindGroup: {
                    SetBindGroupCmd* cmd = mCommands.NextCommand<SetBindGroupCmd>();
                    BindGroup* group = ToBackend(cmd->group.Get());
                    uint32_t* dynamicOffsets = nullptr;

                    if (cmd->dynamicOffsetCount > 0) {
                        dynamicOffsets = mCommands.NextData<uint32_t>(cmd->dynamicOffsetCount);
                    }

                    bindingTracker->OnSetBindGroup(cmd->index, group, cmd->dynamicOffsetCount,
                                                   dynamicOffsets);
                    break;
                }

                case Command::InsertDebugMarker: {
                    InsertDebugMarkerCmd* cmd = mCommands.NextCommand<InsertDebugMarkerCmd>();
                    const char* label = mCommands.NextData<char>(cmd->length + 1);

                    if (ToBackend(GetDevice())->GetFunctions()->IsPIXEventRuntimeLoaded()) {
                        // PIX color is 1 byte per channel in ARGB format
                        constexpr uint64_t kPIXBlackColor = 0xff000000;
                        ToBackend(GetDevice())
                            ->GetFunctions()
                            ->pixSetMarkerOnCommandList(commandList, kPIXBlackColor, label);
                    }
                    break;
                }

                case Command::PopDebugGroup: {
                    mCommands.NextCommand<PopDebugGroupCmd>();

                    if (ToBackend(GetDevice())->GetFunctions()->IsPIXEventRuntimeLoaded()) {
                        ToBackend(GetDevice())
                            ->GetFunctions()
                            ->pixEndEventOnCommandList(commandList);
                    }
                    break;
                }

                case Command::PushDebugGroup: {
                    PushDebugGroupCmd* cmd = mCommands.NextCommand<PushDebugGroupCmd>();
                    const char* label = mCommands.NextData<char>(cmd->length + 1);

                    if (ToBackend(GetDevice())->GetFunctions()->IsPIXEventRuntimeLoaded()) {
                        // PIX color is 1 byte per channel in ARGB format
                        constexpr uint64_t kPIXBlackColor = 0xff000000;
                        ToBackend(GetDevice())
                            ->GetFunctions()
                            ->pixBeginEventOnCommandList(commandList, kPIXBlackColor, label);
                    }
                    break;
                }

                default: {
                    UNREACHABLE();
                    break;
                }
            }
        }

        return {};
    }

    MaybeError CommandBuffer::RecordRayTracingPass(CommandRecordingContext* commandContext,
                                                   BindGroupStateTracker* bindingTracker) {
        PipelineLayout* lastLayout = nullptr;
        RayTracingPipeline* usedPipeline = nullptr;

        ID3D12GraphicsCommandList* commandList = commandContext->GetCommandList();
        ID3D12GraphicsCommandList4* commandList4 = commandContext->GetCommandList4();

        Command type;
        while (mCommands.NextCommandId(&type)) {
            switch (type) {
                case Command::TraceRays: {
                    TraceRaysCmd* traceRays = mCommands.NextCommand<TraceRaysCmd>();

                    ASSERT(usedPipeline != nullptr);

                    RayTracingShaderBindingTable* sbt =
                        ToBackend(usedPipeline->GetShaderBindingTable());

                    uint32_t sbtTableSize = sbt->GetTableSize();
                    ComPtr<ID3D12Resource> sbtTableBuffer = sbt->GetTableBuffer();
                    D3D12_GPU_VIRTUAL_ADDRESS sbtTableBufferAddress =
                        sbtTableBuffer.Get()->GetGPUVirtualAddress();

                    DAWN_TRY(bindingTracker->Apply(commandContext));

                    D3D12_DISPATCH_RAYS_DESC desc;
                    desc.Width = traceRays->width;
                    desc.Height = traceRays->height;
                    desc.Depth = traceRays->depth;

                    // CALL
                    desc.CallableShaderTable.SizeInBytes = 0;
                    desc.CallableShaderTable.StrideInBytes = 0;
                    desc.CallableShaderTable.StartAddress = 0;

                    // RGEN
                    size_t genOffset = traceRays->rayGenerationOffset * sbtTableSize;
                    desc.RayGenerationShaderRecord.StartAddress = sbtTableBufferAddress + genOffset;
                    desc.RayGenerationShaderRecord.SizeInBytes = sbtTableSize;

                    // RHIT
                    size_t hitOffset = traceRays->rayHitOffset * sbtTableSize;
                    desc.HitGroupTable.StartAddress = sbtTableBufferAddress + hitOffset;
                    desc.HitGroupTable.StrideInBytes = sbtTableSize;
                    desc.HitGroupTable.SizeInBytes = sbtTableSize;

                    // RMISS
                    size_t missOffset = traceRays->rayMissOffset * sbtTableSize;
                    desc.MissShaderTable.StartAddress = sbtTableBufferAddress + missOffset;
                    desc.MissShaderTable.StrideInBytes = sbtTableSize;
                    desc.MissShaderTable.SizeInBytes = sbtTableSize;

                    commandList4->DispatchRays(&desc);
                } break;

                case Command::EndRayTracingPass: {
                    mCommands.NextCommand<EndRayTracingPassCmd>();
                    return {};
                } break;

                case Command::SetRayTracingPipeline: {
                    SetRayTracingPipelineCmd* cmd =
                        mCommands.NextCommand<SetRayTracingPipelineCmd>();

                    RayTracingPipeline* pipeline = ToBackend(cmd->pipeline).Get();
                    PipelineLayout* layout = ToBackend(pipeline->GetLayout());

                    commandList->SetComputeRootSignature(layout->GetRootSignature());
                    commandList4->SetPipelineState1(pipeline->GetPipelineState());

                    bindingTracker->OnSetPipeline(pipeline);

                    lastLayout = layout;
                    usedPipeline = pipeline;
                } break;

                case Command::SetBindGroup: {
                    SetBindGroupCmd* cmd = mCommands.NextCommand<SetBindGroupCmd>();
                    BindGroup* group = ToBackend(cmd->group.Get());
                    uint32_t* dynamicOffsets = nullptr;

                    if (cmd->dynamicOffsetCount > 0) {
                        dynamicOffsets = mCommands.NextData<uint32_t>(cmd->dynamicOffsetCount);
                    }

                    bindingTracker->OnSetBindGroup(cmd->index, group, cmd->dynamicOffsetCount,
                                                   dynamicOffsets);
                } break;

                case Command::InsertDebugMarker: {
                    InsertDebugMarkerCmd* cmd = mCommands.NextCommand<InsertDebugMarkerCmd>();
                    const char* label = mCommands.NextData<char>(cmd->length + 1);

                    if (ToBackend(GetDevice())->GetFunctions()->IsPIXEventRuntimeLoaded()) {
                        // PIX color is 1 byte per channel in ARGB format
                        constexpr uint64_t kPIXBlackColor = 0xff000000;
                        ToBackend(GetDevice())
                            ->GetFunctions()
                            ->pixSetMarkerOnCommandList(commandList, kPIXBlackColor, label);
                    }
                } break;

                case Command::PopDebugGroup: {
                    mCommands.NextCommand<PopDebugGroupCmd>();

                    if (ToBackend(GetDevice())->GetFunctions()->IsPIXEventRuntimeLoaded()) {
                        ToBackend(GetDevice())
                            ->GetFunctions()
                            ->pixEndEventOnCommandList(commandList);
                    }
                } break;

                case Command::PushDebugGroup: {
                    PushDebugGroupCmd* cmd = mCommands.NextCommand<PushDebugGroupCmd>();
                    const char* label = mCommands.NextData<char>(cmd->length + 1);

                    if (ToBackend(GetDevice())->GetFunctions()->IsPIXEventRuntimeLoaded()) {
                        // PIX color is 1 byte per channel in ARGB format
                        constexpr uint64_t kPIXBlackColor = 0xff000000;
                        ToBackend(GetDevice())
                            ->GetFunctions()
                            ->pixBeginEventOnCommandList(commandList, kPIXBlackColor, label);
                    }
                } break;

                default: { UNREACHABLE(); } break;
            }
        }

        return {};
    }

    MaybeError CommandBuffer::SetupRenderPass(CommandRecordingContext* commandContext,
                                              BeginRenderPassCmd* renderPass,
                                              RenderPassBuilder* renderPassBuilder) {
        Device* device = ToBackend(GetDevice());
        for (uint32_t i : IterateBitSet(renderPass->attachmentState->GetColorAttachmentsMask())) {
            RenderPassColorAttachmentInfo& attachmentInfo = renderPass->colorAttachments[i];
            TextureView* view = ToBackend(attachmentInfo.view.Get());

            // Set view attachment.
            CPUDescriptorHeapAllocation rtvAllocation;
            DAWN_TRY_ASSIGN(
                rtvAllocation,
                device->GetRenderTargetViewAllocator()->AllocateTransientCPUDescriptors());

            const D3D12_RENDER_TARGET_VIEW_DESC viewDesc = view->GetRTVDescriptor();
            const D3D12_CPU_DESCRIPTOR_HANDLE baseDescriptor = rtvAllocation.GetBaseDescriptor();

            device->GetD3D12Device()->CreateRenderTargetView(
                ToBackend(view->GetTexture())->GetD3D12Resource(), &viewDesc, baseDescriptor);

            renderPassBuilder->SetRenderTargetView(i, baseDescriptor);

            // Set color load operation.
            renderPassBuilder->SetRenderTargetBeginningAccess(
                i, attachmentInfo.loadOp, attachmentInfo.clearColor, view->GetD3D12Format());

            // Set color store operation.
            if (attachmentInfo.resolveTarget.Get() != nullptr) {
                TextureView* resolveDestinationView = ToBackend(attachmentInfo.resolveTarget.Get());
                Texture* resolveDestinationTexture =
                    ToBackend(resolveDestinationView->GetTexture());

                resolveDestinationTexture->TrackUsageAndTransitionNow(
                    commandContext, D3D12_RESOURCE_STATE_RESOLVE_DEST);

                renderPassBuilder->SetRenderTargetEndingAccessResolve(i, attachmentInfo.storeOp,
                                                                      view, resolveDestinationView);
            } else {
                renderPassBuilder->SetRenderTargetEndingAccess(i, attachmentInfo.storeOp);
            }
        }

        if (renderPass->attachmentState->HasDepthStencilAttachment()) {
            RenderPassDepthStencilAttachmentInfo& attachmentInfo =
                renderPass->depthStencilAttachment;
            TextureView* view = ToBackend(renderPass->depthStencilAttachment.view.Get());

            // Set depth attachment.
            CPUDescriptorHeapAllocation dsvAllocation;
            DAWN_TRY_ASSIGN(
                dsvAllocation,
                device->GetDepthStencilViewAllocator()->AllocateTransientCPUDescriptors());

            const D3D12_DEPTH_STENCIL_VIEW_DESC viewDesc = view->GetDSVDescriptor();
            const D3D12_CPU_DESCRIPTOR_HANDLE baseDescriptor = dsvAllocation.GetBaseDescriptor();

            device->GetD3D12Device()->CreateDepthStencilView(
                ToBackend(view->GetTexture())->GetD3D12Resource(), &viewDesc, baseDescriptor);

            renderPassBuilder->SetDepthStencilView(baseDescriptor);

            const bool hasDepth = view->GetTexture()->GetFormat().HasDepth();
            const bool hasStencil = view->GetTexture()->GetFormat().HasStencil();

            // Set depth/stencil load operations.
            if (hasDepth) {
                renderPassBuilder->SetDepthAccess(
                    attachmentInfo.depthLoadOp, attachmentInfo.depthStoreOp,
                    attachmentInfo.clearDepth, view->GetD3D12Format());
            } else {
                renderPassBuilder->SetDepthNoAccess();
            }

            if (hasStencil) {
                renderPassBuilder->SetStencilAccess(
                    attachmentInfo.stencilLoadOp, attachmentInfo.stencilStoreOp,
                    attachmentInfo.clearStencil, view->GetD3D12Format());
            } else {
                renderPassBuilder->SetStencilNoAccess();
            }

        } else {
            renderPassBuilder->SetDepthStencilNoAccess();
        }

        return {};
    }

    void CommandBuffer::EmulateBeginRenderPass(CommandRecordingContext* commandContext,
                                               const RenderPassBuilder* renderPassBuilder) const {
        ID3D12GraphicsCommandList* commandList = commandContext->GetCommandList();

        // Clear framebuffer attachments as needed.
        {
            for (uint32_t i = 0; i < renderPassBuilder->GetColorAttachmentCount(); i++) {
                // Load op - color
                if (renderPassBuilder->GetRenderPassRenderTargetDescriptors()[i]
                        .BeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
                    commandList->ClearRenderTargetView(
                        renderPassBuilder->GetRenderPassRenderTargetDescriptors()[i].cpuDescriptor,
                        renderPassBuilder->GetRenderPassRenderTargetDescriptors()[i]
                            .BeginningAccess.Clear.ClearValue.Color,
                        0, nullptr);
                }
            }

            if (renderPassBuilder->HasDepth()) {
                D3D12_CLEAR_FLAGS clearFlags = {};
                float depthClear = 0.0f;
                uint8_t stencilClear = 0u;

                if (renderPassBuilder->GetRenderPassDepthStencilDescriptor()
                        ->DepthBeginningAccess.Type ==
                    D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
                    clearFlags |= D3D12_CLEAR_FLAG_DEPTH;
                    depthClear = renderPassBuilder->GetRenderPassDepthStencilDescriptor()
                                     ->DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth;
                }
                if (renderPassBuilder->GetRenderPassDepthStencilDescriptor()
                        ->StencilBeginningAccess.Type ==
                    D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
                    clearFlags |= D3D12_CLEAR_FLAG_STENCIL;
                    stencilClear =
                        renderPassBuilder->GetRenderPassDepthStencilDescriptor()
                            ->StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil;
                }

                // TODO(kainino@chromium.org): investigate: should the Dawn clear
                // stencil type be uint8_t?
                if (clearFlags) {
                    commandList->ClearDepthStencilView(
                        renderPassBuilder->GetRenderPassDepthStencilDescriptor()->cpuDescriptor,
                        clearFlags, depthClear, stencilClear, 0, nullptr);
                }
            }
        }

        commandList->OMSetRenderTargets(
            renderPassBuilder->GetColorAttachmentCount(), renderPassBuilder->GetRenderTargetViews(),
            FALSE,
            renderPassBuilder->HasDepth()
                ? &renderPassBuilder->GetRenderPassDepthStencilDescriptor()->cpuDescriptor
                : nullptr);
    }

    MaybeError CommandBuffer::RecordRenderPass(CommandRecordingContext* commandContext,
                                               BindGroupStateTracker* bindingTracker,
                                               BeginRenderPassCmd* renderPass,
                                               const bool passHasUAV) {
        Device* device = ToBackend(GetDevice());
        const bool useRenderPass = device->IsToggleEnabled(Toggle::UseD3D12RenderPass);

        // renderPassBuilder must be scoped to RecordRenderPass because any underlying
        // D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS structs must remain
        // valid until after EndRenderPass() has been called.
        RenderPassBuilder renderPassBuilder(passHasUAV);

        DAWN_TRY(SetupRenderPass(commandContext, renderPass, &renderPassBuilder));

        // Use D3D12's native render pass API if it's available, otherwise emulate the
        // beginning and ending access operations.
        if (useRenderPass) {
            commandContext->GetCommandList4()->BeginRenderPass(
                renderPassBuilder.GetColorAttachmentCount(),
                renderPassBuilder.GetRenderPassRenderTargetDescriptors(),
                renderPassBuilder.HasDepth()
                    ? renderPassBuilder.GetRenderPassDepthStencilDescriptor()
                    : nullptr,
                renderPassBuilder.GetRenderPassFlags());
        } else {
            EmulateBeginRenderPass(commandContext, &renderPassBuilder);
        }

        ID3D12GraphicsCommandList* commandList = commandContext->GetCommandList();

        // Set up default dynamic state
        {
            uint32_t width = renderPass->width;
            uint32_t height = renderPass->height;
            D3D12_VIEWPORT viewport = {
                0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
            D3D12_RECT scissorRect = {0, 0, static_cast<long>(width), static_cast<long>(height)};
            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissorRect);

            static constexpr std::array<float, 4> defaultBlendFactor = {0, 0, 0, 0};
            commandList->OMSetBlendFactor(&defaultBlendFactor[0]);
        }

        RenderPipeline* lastPipeline = nullptr;
        PipelineLayout* lastLayout = nullptr;
        VertexBufferTracker vertexBufferTracker = {};
        IndexBufferTracker indexBufferTracker = {};

        auto EncodeRenderBundleCommand = [&](CommandIterator* iter, Command type) -> MaybeError {
            switch (type) {
                case Command::Draw: {
                    DrawCmd* draw = iter->NextCommand<DrawCmd>();

                    DAWN_TRY(bindingTracker->Apply(commandContext));
                    vertexBufferTracker.Apply(commandList, lastPipeline);
                    commandList->DrawInstanced(draw->vertexCount, draw->instanceCount,
                                               draw->firstVertex, draw->firstInstance);
                    break;
                }

                case Command::DrawIndexed: {
                    DrawIndexedCmd* draw = iter->NextCommand<DrawIndexedCmd>();

                    DAWN_TRY(bindingTracker->Apply(commandContext));
                    indexBufferTracker.Apply(commandList);
                    vertexBufferTracker.Apply(commandList, lastPipeline);
                    commandList->DrawIndexedInstanced(draw->indexCount, draw->instanceCount,
                                                      draw->firstIndex, draw->baseVertex,
                                                      draw->firstInstance);
                    break;
                }

                case Command::DrawIndirect: {
                    DrawIndirectCmd* draw = iter->NextCommand<DrawIndirectCmd>();

                    DAWN_TRY(bindingTracker->Apply(commandContext));
                    vertexBufferTracker.Apply(commandList, lastPipeline);
                    Buffer* buffer = ToBackend(draw->indirectBuffer.Get());
                    ComPtr<ID3D12CommandSignature> signature =
                        ToBackend(GetDevice())->GetDrawIndirectSignature();
                    commandList->ExecuteIndirect(signature.Get(), 1,
                                                 buffer->GetD3D12Resource().Get(),
                                                 draw->indirectOffset, nullptr, 0);
                    break;
                }

                case Command::DrawIndexedIndirect: {
                    DrawIndexedIndirectCmd* draw = iter->NextCommand<DrawIndexedIndirectCmd>();

                    DAWN_TRY(bindingTracker->Apply(commandContext));
                    indexBufferTracker.Apply(commandList);
                    vertexBufferTracker.Apply(commandList, lastPipeline);
                    Buffer* buffer = ToBackend(draw->indirectBuffer.Get());
                    ComPtr<ID3D12CommandSignature> signature =
                        ToBackend(GetDevice())->GetDrawIndexedIndirectSignature();
                    commandList->ExecuteIndirect(signature.Get(), 1,
                                                 buffer->GetD3D12Resource().Get(),
                                                 draw->indirectOffset, nullptr, 0);
                    break;
                }

                case Command::InsertDebugMarker: {
                    InsertDebugMarkerCmd* cmd = iter->NextCommand<InsertDebugMarkerCmd>();
                    const char* label = iter->NextData<char>(cmd->length + 1);

                    if (ToBackend(GetDevice())->GetFunctions()->IsPIXEventRuntimeLoaded()) {
                        // PIX color is 1 byte per channel in ARGB format
                        constexpr uint64_t kPIXBlackColor = 0xff000000;
                        ToBackend(GetDevice())
                            ->GetFunctions()
                            ->pixSetMarkerOnCommandList(commandList, kPIXBlackColor, label);
                    }
                    break;
                }

                case Command::PopDebugGroup: {
                    iter->NextCommand<PopDebugGroupCmd>();

                    if (ToBackend(GetDevice())->GetFunctions()->IsPIXEventRuntimeLoaded()) {
                        ToBackend(GetDevice())
                            ->GetFunctions()
                            ->pixEndEventOnCommandList(commandList);
                    }
                    break;
                }

                case Command::PushDebugGroup: {
                    PushDebugGroupCmd* cmd = iter->NextCommand<PushDebugGroupCmd>();
                    const char* label = iter->NextData<char>(cmd->length + 1);

                    if (ToBackend(GetDevice())->GetFunctions()->IsPIXEventRuntimeLoaded()) {
                        // PIX color is 1 byte per channel in ARGB format
                        constexpr uint64_t kPIXBlackColor = 0xff000000;
                        ToBackend(GetDevice())
                            ->GetFunctions()
                            ->pixBeginEventOnCommandList(commandList, kPIXBlackColor, label);
                    }
                    break;
                }

                case Command::SetRenderPipeline: {
                    SetRenderPipelineCmd* cmd = iter->NextCommand<SetRenderPipelineCmd>();
                    RenderPipeline* pipeline = ToBackend(cmd->pipeline).Get();
                    PipelineLayout* layout = ToBackend(pipeline->GetLayout());

                    commandList->SetGraphicsRootSignature(layout->GetRootSignature());
                    commandList->SetPipelineState(pipeline->GetPipelineState());
                    commandList->IASetPrimitiveTopology(pipeline->GetD3D12PrimitiveTopology());

                    bindingTracker->OnSetPipeline(pipeline);
                    indexBufferTracker.OnSetPipeline(pipeline);

                    lastPipeline = pipeline;
                    lastLayout = layout;
                    break;
                }

                case Command::SetBindGroup: {
                    SetBindGroupCmd* cmd = iter->NextCommand<SetBindGroupCmd>();
                    BindGroup* group = ToBackend(cmd->group.Get());
                    uint32_t* dynamicOffsets = nullptr;

                    if (cmd->dynamicOffsetCount > 0) {
                        dynamicOffsets = iter->NextData<uint32_t>(cmd->dynamicOffsetCount);
                    }

                    bindingTracker->OnSetBindGroup(cmd->index, group, cmd->dynamicOffsetCount,
                                                   dynamicOffsets);
                    break;
                }

                case Command::SetIndexBuffer: {
                    SetIndexBufferCmd* cmd = iter->NextCommand<SetIndexBufferCmd>();

                    indexBufferTracker.OnSetIndexBuffer(ToBackend(cmd->buffer.Get()), cmd->offset,
                                                        cmd->size);
                    break;
                }

                case Command::SetVertexBuffer: {
                    SetVertexBufferCmd* cmd = iter->NextCommand<SetVertexBufferCmd>();

                    vertexBufferTracker.OnSetVertexBuffer(cmd->slot, ToBackend(cmd->buffer.Get()),
                                                          cmd->offset, cmd->size);
                    break;
                }

                default:
                    UNREACHABLE();
                    break;
            }
            return {};
        };

        Command type;
        while (mCommands.NextCommandId(&type)) {
            switch (type) {
                case Command::EndRenderPass: {
                    mCommands.NextCommand<EndRenderPassCmd>();
                    if (useRenderPass) {
                        commandContext->GetCommandList4()->EndRenderPass();
                    } else if (renderPass->attachmentState->GetSampleCount() > 1) {
                        ResolveMultisampledRenderPass(commandContext, renderPass);
                    }
                    return {};
                }

                case Command::SetStencilReference: {
                    SetStencilReferenceCmd* cmd = mCommands.NextCommand<SetStencilReferenceCmd>();

                    commandList->OMSetStencilRef(cmd->reference);
                    break;
                }

                case Command::SetViewport: {
                    SetViewportCmd* cmd = mCommands.NextCommand<SetViewportCmd>();
                    D3D12_VIEWPORT viewport;
                    viewport.TopLeftX = cmd->x;
                    viewport.TopLeftY = cmd->y;
                    viewport.Width = cmd->width;
                    viewport.Height = cmd->height;
                    viewport.MinDepth = cmd->minDepth;
                    viewport.MaxDepth = cmd->maxDepth;

                    commandList->RSSetViewports(1, &viewport);
                    break;
                }

                case Command::SetScissorRect: {
                    SetScissorRectCmd* cmd = mCommands.NextCommand<SetScissorRectCmd>();
                    D3D12_RECT rect;
                    rect.left = cmd->x;
                    rect.top = cmd->y;
                    rect.right = cmd->x + cmd->width;
                    rect.bottom = cmd->y + cmd->height;

                    commandList->RSSetScissorRects(1, &rect);
                    break;
                }

                case Command::SetBlendColor: {
                    SetBlendColorCmd* cmd = mCommands.NextCommand<SetBlendColorCmd>();
                    commandList->OMSetBlendFactor(static_cast<const FLOAT*>(&cmd->color.r));
                    break;
                }

                case Command::ExecuteBundles: {
                    ExecuteBundlesCmd* cmd = mCommands.NextCommand<ExecuteBundlesCmd>();
                    auto bundles = mCommands.NextData<Ref<RenderBundleBase>>(cmd->count);

                    for (uint32_t i = 0; i < cmd->count; ++i) {
                        CommandIterator* iter = bundles[i]->GetCommands();
                        iter->Reset();
                        while (iter->NextCommandId(&type)) {
                            DAWN_TRY(EncodeRenderBundleCommand(iter, type));
                        }
                    }
                    break;
                }

                default: {
                    DAWN_TRY(EncodeRenderBundleCommand(&mCommands, type));
                    break;
                }
            }
        }
        return {};
    }
}}  // namespace dawn_native::d3d12
