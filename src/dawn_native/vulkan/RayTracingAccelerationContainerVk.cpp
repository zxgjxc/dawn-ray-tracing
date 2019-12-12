// Copyright 2018 The Dawn Authors
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

#include "dawn_native/vulkan/RayTracingAccelerationContainerVk.h"
#include "dawn_native/vulkan/RayTracingAccelerationGeometryVk.h"
#include "dawn_native/vulkan/RayTracingAccelerationInstanceVk.h"

#include "dawn_native/vulkan/DeviceVk.h"
#include "dawn_native/vulkan/VulkanError.h"

namespace dawn_native { namespace vulkan {

    namespace {

        VkAccelerationStructureTypeNV VulkanAccelerationContainerLevel(wgpu::RayTracingAccelerationContainerLevel containerLevel) {
            VkAccelerationStructureTypeNV level = static_cast<VkAccelerationStructureTypeNV>(0);
            if (containerLevel == wgpu::RayTracingAccelerationContainerLevel::Bottom) {
                level = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_NV;
            }
            if (containerLevel == wgpu::RayTracingAccelerationContainerLevel::Top) {
                level = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV;
            }
            return level;
        }

        VkBuildAccelerationStructureFlagBitsNV VulkanBuildAccelerationStructureFlags(wgpu::RayTracingAccelerationContainerFlag buildFlags) {
            uint32_t flags = 0;
            if (buildFlags & wgpu::RayTracingAccelerationContainerFlag::AllowUpdate) {
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_NV;
            }
            if (buildFlags & wgpu::RayTracingAccelerationContainerFlag::PreferFastBuild) {
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_NV;
            }
            if (buildFlags & wgpu::RayTracingAccelerationContainerFlag::PreferFastTrace) {
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_NV;
            }
            if (buildFlags & wgpu::RayTracingAccelerationContainerFlag::LowMemory) {
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_NV;
            }
            return static_cast<VkBuildAccelerationStructureFlagBitsNV>(flags);
        }

    }  // anonymous namespace

    // static
    ResultOrError<RayTracingAccelerationContainer*> RayTracingAccelerationContainer::Create(Device* device, const RayTracingAccelerationContainerDescriptor* descriptor) {
        std::unique_ptr<RayTracingAccelerationContainer> geometry = std::make_unique<RayTracingAccelerationContainer>(device, descriptor);
        DAWN_TRY(geometry->Initialize(descriptor));
        return geometry.release();
    }

    MaybeError RayTracingAccelerationContainer::Initialize(const RayTracingAccelerationContainerDescriptor* descriptor) {
        Device* device = ToBackend(GetDevice());

        std::vector<VkGeometryNV> geometries;
        for (unsigned int ii = 0; ii < descriptor->geometryCount; ++ii) {
            VkGeometryNV geometry = static_cast<RayTracingAccelerationGeometry*>(descriptor->geometries[ii])->GetInfo();
            geometries.push_back(geometry);
        };

        VkAccelerationStructureInfoNV accelerationStructureInfo{};
        accelerationStructureInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV;
        accelerationStructureInfo.flags = VulkanBuildAccelerationStructureFlags(descriptor->flags);
        accelerationStructureInfo.type = VulkanAccelerationContainerLevel(descriptor->level);
        if (descriptor->level == wgpu::RayTracingAccelerationContainerLevel::Top) {
            accelerationStructureInfo.geometryCount = 0;
            accelerationStructureInfo.instanceCount = descriptor->instanceCount;
        } else if (descriptor->level == wgpu::RayTracingAccelerationContainerLevel::Bottom) {
            accelerationStructureInfo.instanceCount = 0;
            accelerationStructureInfo.geometryCount = descriptor->geometryCount;
            accelerationStructureInfo.pGeometries = geometries.data();
        } else {
            return DAWN_VALIDATION_ERROR("Invalid Acceleration Container Level");
        }
        // save container level
        mLevel = accelerationStructureInfo.type;

        VkAccelerationStructureCreateInfoNV accelerationStructureCI{};
        accelerationStructureCI.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_NV;
        accelerationStructureCI.info = accelerationStructureInfo;

        // validate ray tracing calls
        if (device->fn.CreateAccelerationStructureNV == nullptr) {
            return DAWN_VALIDATION_ERROR("Invalid Call to CreateAccelerationStructureNV");
        }
        if (device->fn.GetAccelerationStructureHandleNV == nullptr) {
            return DAWN_VALIDATION_ERROR("Invalid Call to GetAccelerationStructureHandleNV");
        }
        if (device->fn.GetAccelerationStructureMemoryRequirementsNV == nullptr) {
            return DAWN_VALIDATION_ERROR("Invalid Call to GetAccelerationStructureMemoryRequirementsNV");
        }

        {
            MaybeError result = CheckVkSuccess(
              device->fn.CreateAccelerationStructureNV(
                  device->GetVkDevice(), &accelerationStructureCI, nullptr, &mAccelerationStructure),
              "CreateAccelerationStructureNV");
            if (result.IsError()) return result.AcquireError();
        }

        {
            MaybeError result = CheckVkSuccess(
                device->fn.GetAccelerationStructureHandleNV(
                    device->GetVkDevice(), mAccelerationStructure, sizeof(void*), &mHandle),
                "GetAccelerationStructureHandleNV");
            if (result.IsError()) return result.AcquireError();
        }

        // acceleration container holds instances to geometry
        if (GetLevel() == VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV) {

            // find all unique bottom-level containers
            std::vector<RayTracingAccelerationContainer*> geometryContainers;
            for (unsigned int ii = 0; ii < descriptor->instanceCount; ++ii) {
                RayTracingAccelerationInstance* instance = static_cast<RayTracingAccelerationInstance*>(descriptor->instances[ii]);
                RayTracingAccelerationContainer* container = instance->GetGeometryContainer();
                if (container == nullptr) {
                    return DAWN_VALIDATION_ERROR(
                        "Invalid Reference to RayTracingAccelerationContainer");
                }
                // only pick unique geometry containers
                if (std::find(geometryContainers.begin(), geometryContainers.end(), container) == geometryContainers.end()) {
                    geometryContainers.push_back(container);
                }
            };
            // find scratch buffer dimensions
            uint32_t scratchBufferResultOffset = 0;
            uint32_t scratchBufferBuildOffset = 0;
            uint32_t scratchBufferUpdateOffset = 0;
            for (unsigned int ii = 0; ii < geometryContainers.size(); ++ii) {
                RayTracingAccelerationContainer* container = geometryContainers[ii];
                uint32_t resultSize = container->GetMemoryRequirementSize(
                    VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_OBJECT_NV);
                uint32_t buildSize = container->GetMemoryRequirementSize(
                    VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_BUILD_SCRATCH_NV);
                uint32_t updateSize = container->GetMemoryRequirementSize(
                    VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_UPDATE_SCRATCH_NV);
            };
        }

        return {};
    }

    RayTracingAccelerationContainer::~RayTracingAccelerationContainer() {
        Device* device = ToBackend(GetDevice());
        if (mAccelerationStructure != VK_NULL_HANDLE) {
            device->fn.DestroyAccelerationStructureNV(device->GetVkDevice(), mAccelerationStructure, nullptr);
            mAccelerationStructure = VK_NULL_HANDLE;
        }
    }

    uint64_t RayTracingAccelerationContainer::GetHandle() const {
        return mHandle;
    }

    VkAccelerationStructureTypeNV RayTracingAccelerationContainer::GetLevel() const {
        return mLevel;
    }

    VkAccelerationStructureNV RayTracingAccelerationContainer::GetAccelerationStructure() const {
        return mAccelerationStructure;
    }

    uint32_t RayTracingAccelerationContainer::GetMemoryRequirementSize(
        VkAccelerationStructureMemoryRequirementsTypeNV type) const {
        Device* device = ToBackend(GetDevice());

        VkAccelerationStructureMemoryRequirementsInfoNV memoryRequirementsInfo{};
        memoryRequirementsInfo.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_INFO_NV;
        memoryRequirementsInfo.accelerationStructure = mAccelerationStructure;

        VkMemoryRequirements2 memoryRequirements2{};
        memoryRequirements2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;

        memoryRequirementsInfo.type = type;
        device->fn.GetAccelerationStructureMemoryRequirementsNV(
            device->GetVkDevice(), &memoryRequirementsInfo, &memoryRequirements2);
        return memoryRequirements2.memoryRequirements.size;
    }

}}  // namespace dawn_native::vulkan
