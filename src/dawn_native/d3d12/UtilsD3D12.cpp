// Copyright 2019 The Dawn Authors
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

#include "dawn_native/d3d12/UtilsD3D12.h"

#include "common/Assert.h"

namespace dawn_native { namespace d3d12 {

    D3D12_COMPARISON_FUNC ToD3D12ComparisonFunc(wgpu::CompareFunction func) {
        switch (func) {
            case wgpu::CompareFunction::Always:
                return D3D12_COMPARISON_FUNC_ALWAYS;
            case wgpu::CompareFunction::Equal:
                return D3D12_COMPARISON_FUNC_EQUAL;
            case wgpu::CompareFunction::Greater:
                return D3D12_COMPARISON_FUNC_GREATER;
            case wgpu::CompareFunction::GreaterEqual:
                return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case wgpu::CompareFunction::Less:
                return D3D12_COMPARISON_FUNC_LESS;
            case wgpu::CompareFunction::LessEqual:
                return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case wgpu::CompareFunction::Never:
                return D3D12_COMPARISON_FUNC_NEVER;
            case wgpu::CompareFunction::NotEqual:
                return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            default:
                UNREACHABLE();
        }
    }

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE ToD3D12RayTracingAccelerationContainerLevel(
        wgpu::RayTracingAccelerationContainerLevel level) {
        switch (level) {
            case wgpu::RayTracingAccelerationContainerLevel::Bottom:
                return D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            case wgpu::RayTracingAccelerationContainerLevel::Top:
                return D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            default:
                UNREACHABLE();
        }
    }

    D3D12_RAYTRACING_GEOMETRY_TYPE ToD3D12RayTracingGeometryType(
        wgpu::RayTracingAccelerationGeometryType geometryType) {
        switch (geometryType) {
            case wgpu::RayTracingAccelerationGeometryType::Triangles:
                return D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            case wgpu::RayTracingAccelerationGeometryType::Aabbs:
                return D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
            default:
                UNREACHABLE();
        }
    }

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS
    ToD3D12RayTracingAccelerationStructureBuildFlags(
        wgpu::RayTracingAccelerationContainerFlag buildFlags) {
        uint32_t flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
        if (buildFlags & wgpu::RayTracingAccelerationContainerFlag::AllowUpdate) {
            flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
        }
        if (buildFlags & wgpu::RayTracingAccelerationContainerFlag::PreferFastBuild) {
            flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        }
        if (buildFlags & wgpu::RayTracingAccelerationContainerFlag::PreferFastTrace) {
            flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        }
        if (buildFlags & wgpu::RayTracingAccelerationContainerFlag::LowMemory) {
            flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_MINIMIZE_MEMORY;
        }
        return static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(flags);
    }

    D3D12_RAYTRACING_GEOMETRY_FLAGS ToD3D12RayTracingGeometryFlags(
        wgpu::RayTracingAccelerationGeometryFlag geometryFlags) {
        uint32_t flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        if (geometryFlags & wgpu::RayTracingAccelerationGeometryFlag::Opaque) {
            flags |= D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        }
        if (geometryFlags & wgpu::RayTracingAccelerationGeometryFlag::AllowAnyHit) {
            flags |= D3D12_RAYTRACING_GEOMETRY_FLAG_NO_DUPLICATE_ANYHIT_INVOCATION;
        }
        return static_cast<D3D12_RAYTRACING_GEOMETRY_FLAGS>(flags);
    }

    D3D12_RAYTRACING_INSTANCE_FLAGS ToD3D12RayTracingInstanceFlags(
        wgpu::RayTracingAccelerationInstanceFlag instanceFlags) {
        uint32_t flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        if (instanceFlags & wgpu::RayTracingAccelerationInstanceFlag::TriangleCullDisable) {
            flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        }
        if (instanceFlags &
            wgpu::RayTracingAccelerationInstanceFlag::TriangleFrontCounterclockwise) {
            flags |= D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
        }
        if (instanceFlags & wgpu::RayTracingAccelerationInstanceFlag::ForceOpaque) {
            flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
        }
        if (instanceFlags & wgpu::RayTracingAccelerationInstanceFlag::ForceNoOpaque) {
            flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
        }
        return static_cast<D3D12_RAYTRACING_INSTANCE_FLAGS>(flags);
    }

    D3D12_TEXTURE_COPY_LOCATION ComputeTextureCopyLocationForTexture(const Texture* texture,
                                                                     uint32_t level,
                                                                     uint32_t slice) {
        D3D12_TEXTURE_COPY_LOCATION copyLocation;
        copyLocation.pResource = texture->GetD3D12Resource();
        copyLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        copyLocation.SubresourceIndex = texture->GetSubresourceIndex(level, slice);

        return copyLocation;
    }

    D3D12_TEXTURE_COPY_LOCATION ComputeBufferLocationForCopyTextureRegion(
        const Texture* texture,
        ID3D12Resource* bufferResource,
        const Extent3D& bufferSize,
        const uint64_t offset,
        const uint32_t rowPitch) {
        D3D12_TEXTURE_COPY_LOCATION bufferLocation;
        bufferLocation.pResource = bufferResource;
        bufferLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        bufferLocation.PlacedFootprint.Offset = offset;
        bufferLocation.PlacedFootprint.Footprint.Format = texture->GetD3D12Format();
        bufferLocation.PlacedFootprint.Footprint.Width = bufferSize.width;
        bufferLocation.PlacedFootprint.Footprint.Height = bufferSize.height;
        bufferLocation.PlacedFootprint.Footprint.Depth = bufferSize.depth;
        bufferLocation.PlacedFootprint.Footprint.RowPitch = rowPitch;
        return bufferLocation;
    }

    D3D12_BOX ComputeD3D12BoxFromOffsetAndSize(const Origin3D& offset, const Extent3D& copySize) {
        D3D12_BOX sourceRegion;
        sourceRegion.left = offset.x;
        sourceRegion.top = offset.y;
        sourceRegion.front = offset.z;
        sourceRegion.right = offset.x + copySize.width;
        sourceRegion.bottom = offset.y + copySize.height;
        sourceRegion.back = offset.z + copySize.depth;
        return sourceRegion;
    }

}}  // namespace dawn_native::d3d12
