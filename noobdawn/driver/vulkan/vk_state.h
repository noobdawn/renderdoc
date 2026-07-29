/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

#include "vk_common.h"

struct VulkanCreationInfo;
class VulkanResourceManager;
class WrappedVulkan;
struct DescSetLayout;

struct VulkanStatePipeline
{
  VulkanStatePipeline(VkPipelineBindPoint bindPoint) : bindPoint(bindPoint) {}

  VkPipelineBindPoint bindPoint;

  ResourceId pipeline;

  // shader object
  bool shaderObject = false;

  struct DescriptorAndOffsets
  {
    ResourceId pipeLayout;
    bool push = false;

    // if descriptor set bound
    ResourceId descSet;
    nbdarray<uint32_t> offsets;

    // if descriptor buffer bound
    uint32_t descBufferIdx = ~0U;
    VkDeviceSize descBufferOffset = 0;
    bool descBufferEmbeddedSamplers = false;
    bool descBufferPush = false;

    bool IsDescBufferBound() const
    {
      return descBufferIdx != ~0U || descBufferEmbeddedSamplers || descBufferPush;
    }

    bool IsBound() const
    {
      return descSet != ResourceId() || descBufferIdx != ~0U || descBufferEmbeddedSamplers;
    }
  };
  nbdarray<DescriptorAndOffsets> descSets;
  // the index of the last set bound. In the case where we are re-binding sets and don't have a
  // valid pipeline to reference, this can help us resolve which descriptor sets to rebind in the
  // event that they're not all compatible
  // we need to track the last descriptor set separately from the last descriptor buffer set,
  // because if a descriptor buffer set is invalidated the previous last descriptor set may still be
  // valid. There is no way to do the other direction (any descriptor set bind invalidates all
  // descriptor buffer set bindings)
  int32_t lastBoundDescSet = -1;
  int32_t lastBoundDescBufSet = -1;

  uint32_t LastBoundSet() const
  {
    if(UsingDescBufs() && lastBoundDescBufSet >= 0)
      return lastBoundDescBufSet;
    if(lastBoundDescSet >= 0)
      return lastBoundDescSet;
    return 0;
  }

  bool UsingDescBufs() const
  {
    // all sets must be using buffers or not
    return !descSets.empty() && descSets[0].descBufferIdx != ~0U;
  }
};

struct VulkanRenderState
{
  enum PipelineBinding
  {
    BindNone = 0x0,
    BindGraphics = 0x1,
    BindCompute = 0x2,
    BindRT = 0x4,
    BindInitial = 0x8,
  };

  VulkanRenderState();
  bool IsConditionalRenderingEnabled();
  void BeginRenderPassAndApplyState(WrappedVulkan *vk, VkCommandBuffer cmd, PipelineBinding binding,
                                    bool obeySuspending);
  void BindPipeline(WrappedVulkan *vk, VkCommandBuffer cmd, PipelineBinding binding, bool subpass0);

  void BindDescriptorBuffers(WrappedVulkan *vk, VkCommandBuffer cmd);

  void BindShaderObjects(WrappedVulkan *vk, VkCommandBuffer cmd, PipelineBinding binding);
  void BindDynamicState(WrappedVulkan *vk, VkCommandBuffer cmd);

  void EndRenderPass(VkCommandBuffer cmd);
  void FinishSuspendedRenderPass(VkCommandBuffer cmd);

  void EndTransformFeedback(WrappedVulkan *vk, VkCommandBuffer cmd);

  void EndConditionalRendering(VkCommandBuffer cmd);

  // dynamic state - this mask tracks which dynamic states have been set, since we can't rely on the
  // bound pipeline to know which ones to re-bind if we're reapplying state. It's possible to bind
  // a static pipeline then set some dynamic state. If we want to push and pop state at that point
  // (say for a discard pattern) we need to restore the dynamic state even if it's "invalid" for the
  // pipeline, as the application presumably then binds a dynamic pipeline afterwards before drawing
  nbdfixedarray<bool, VkDynamicCount> dynamicStates = {};

  nbdarray<VkViewport> views;
  nbdarray<VkRect2D> scissors;
  float lineWidth = 1.0f;
  struct
  {
    float depth = 0.0f;
    float biasclamp = 0.0f;
    float slope = 0.0f;

    VkDepthBiasRepresentationEXT repr =
        VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORMAT_EXT;
    bool exact = false;
  } bias;
  float blendConst[4] = {};
  float mindepth = 0.0f;
  float maxdepth = 1.0f;
  struct
  {
    uint32_t compare = 0;
    uint32_t write = 0;
    uint32_t ref = 0;

    // extended dynamic state
    VkStencilOp failOp = VK_STENCIL_OP_KEEP, passOp = VK_STENCIL_OP_KEEP,
                depthFailOp = VK_STENCIL_OP_KEEP;
    VkCompareOp compareOp = VK_COMPARE_OP_ALWAYS;
  } front, back;

  struct
  {
    VkSampleCountFlagBits sampleCount;
    VkExtent2D gridSize;
    nbdarray<VkSampleLocationEXT> locations;
  } sampleLocations;

  nbdarray<VkRect2D> discardRectangles;

  // raytracing stack size
  uint32_t rtStackSize = 0;

  uint32_t stippleFactor = 0;
  uint16_t stipplePattern = 0;

  // this should be big enough for any implementation
  byte pushconsts[1024] = {};
  // the actual number of bytes that have been uploaded
  uint32_t pushConstSize = 0;
  // the layout last used to push. Used for handling cases where we are rebinding
  // partial/temporarily invalid state like if we are resetting state after a discard pattern and
  // the command buffer has only pushed some constants but not yet bound a pipeline
  ResourceId pushLayout;

  uint32_t subpass = 0;
  VkSubpassContents subpassContents;

  // helper function - if the pipeline has been changed, this forces all dynamic state to be bound
  // that the pipeline needs (and no other dynamic state). Helpful if the state is mutated just
  // before a draw, before replaying that draw
  void SetDynamicStatesFromPipeline(WrappedVulkan *m_pDriver);

  // framebuffer accessors - to allow for imageless framebuffers and prevent accidentally changing
  // only the framebuffer without updating the attachments
  void SetFramebuffer(WrappedVulkan *vk, ResourceId fb,
                      const VkRenderPassAttachmentBeginInfo *attachmentsInfo = NULL);

  void SetFramebuffer(ResourceId fb, const nbdarray<ResourceId> &dynamicAttachments)
  {
    framebuffer = fb;
    fbattachments = dynamicAttachments;
  }
  ResourceId GetFramebuffer() const { return framebuffer; }
  const nbdarray<ResourceId> &GetFramebufferAttachments() const { return fbattachments; }
  //

  // same for renderpass
  void SetRenderPass(ResourceId rp)
  {
    renderPass = rp;
    dynamicRendering = DynamicRendering();
  }
  ResourceId GetRenderPass() const { return renderPass; }
  bool ActiveRenderPass() const { return renderPass != ResourceId() || dynamicRendering.active; }
  VkRect2D renderArea = {};

  VulkanStatePipeline compute = VulkanStatePipeline(VK_PIPELINE_BIND_POINT_COMPUTE);
  VulkanStatePipeline graphics = VulkanStatePipeline(VK_PIPELINE_BIND_POINT_GRAPHICS);
  VulkanStatePipeline rt = VulkanStatePipeline(VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR);

  VulkanStatePipeline &GetPipeline(VkPipelineBindPoint pipelineBindPoint)
  {
    if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
      return graphics;
    else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR)
      return rt;
    return compute;
  }

  struct DescriptorBuffer
  {
    VkDeviceAddress address;
    VkBufferUsageFlags2 usage;
    bool flags2;
    ResourceId pushBuffer;
  };
  nbdarray<DescriptorBuffer> descBufs;

  struct IdxBuffer
  {
    ResourceId buf;
    VkDeviceSize offs = 0;
    VkDeviceSize size = VK_WHOLE_SIZE;
    int bytewidth = 0;
  } ibuffer;

  struct VertBuffer
  {
    ResourceId buf;
    VkDeviceSize offs = 0;

    // extended dynamic state
    VkDeviceSize size = VK_WHOLE_SIZE;
    VkDeviceSize stride = 0;
  };
  nbdarray<VertBuffer> vbuffers;

  struct XFBBuffer
  {
    ResourceId buf;
    VkDeviceSize offs = 0;
    VkDeviceSize size = 0;
  };
  nbdarray<XFBBuffer> xfbbuffers;

  struct XFBCounter
  {
    ResourceId buf;
    VkDeviceSize offs = 0;
  };
  uint32_t firstxfbcounter = 0;
  nbdarray<XFBCounter> xfbcounters;

  struct ConditionalRendering
  {
    ResourceId buffer;
    VkDeviceSize offset = 0;
    VkConditionalRenderingFlagsEXT flags = 0;

    bool forceDisable = false;
  } conditionalRendering;

  // extended dynamic state
  VkPrimitiveTopology primitiveTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
  VkFrontFace frontFace = VK_FRONT_FACE_CLOCKWISE;

  VkBool32 depthBoundsTestEnable = VK_FALSE;
  VkBool32 depthTestEnable = VK_FALSE;
  VkBool32 depthWriteEnable = VK_FALSE;
  VkCompareOp depthCompareOp = VK_COMPARE_OP_ALWAYS;

  VkBool32 stencilTestEnable = VK_FALSE;

  // color write enable
  nbdarray<VkBool32> colorWriteEnable;

  // extended dynamic state 2
  VkBool32 depthBiasEnable = VK_FALSE;
  VkLogicOp logicOp = VK_LOGIC_OP_CLEAR;
  uint32_t patchControlPoints = 3;
  VkBool32 primRestartEnable = VK_FALSE;
  VkBool32 rastDiscardEnable = VK_FALSE;

  // extended dynamic state 3
  VkBool32 alphaToCoverageEnable = VK_FALSE;
  VkBool32 alphaToOneEnable = VK_FALSE;
  nbdarray<VkBool32> colorBlendEnable;
  nbdarray<VkColorBlendEquationEXT> colorBlendEquation;
  nbdarray<VkColorComponentFlags> colorWriteMask;
  VkConservativeRasterizationModeEXT conservativeRastMode =
      VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT;
  VkBool32 depthClampEnable = VK_FALSE;
  VkBool32 depthClipEnable = VK_FALSE;
  VkBool32 negativeOneToOne = VK_FALSE;
  float primOverestimationSize = 0.0f;
  VkLineRasterizationMode lineRasterMode = VK_LINE_RASTERIZATION_MODE_DEFAULT;
  VkBool32 stippledLineEnable = VK_FALSE;
  VkBool32 logicOpEnable = VK_FALSE;
  VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
  VkProvokingVertexModeEXT provokingVertexMode = VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
  VkSampleCountFlagBits rastSamples = VK_SAMPLE_COUNT_1_BIT;
  uint32_t rasterStream = 0;
  VkBool32 sampleLocEnable = VK_FALSE;
  nbdarray<VkSampleMask> sampleMask = {0};
  VkTessellationDomainOrigin domainOrigin = VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT;

  // dynamic vertex input
  nbdarray<VkVertexInputBindingDescription2EXT> vertexBindings;
  nbdarray<VkVertexInputAttributeDescription2EXT> vertexAttributes;

  // dynamic rendering
  struct DynamicRendering
  {
    DynamicRendering() = default;
    DynamicRendering &operator=(const DynamicRendering &o)
    {
      active = o.active;
      suspended = o.suspended;
      flags = o.flags;
      layerCount = o.layerCount;
      viewMask = o.viewMask;
      color = o.color;
      depth = o.depth;
      stencil = o.stencil;

      fragmentDensityView = o.fragmentDensityView;
      fragmentDensityLayout = o.fragmentDensityLayout;

      shadingRateView = o.shadingRateView;
      shadingRateLayout = o.shadingRateLayout;
      shadingRateTexelSize = o.shadingRateTexelSize;

      tileOnlyMSAAEnable = o.tileOnlyMSAAEnable;
      tileOnlyMSAASampleCount = o.tileOnlyMSAASampleCount;

      localRead = o.localRead;

      beginCustomResolve = o.beginCustomResolve;

      // this will deep copy from the incoming object
      CopyAttachmentNexts();

      return *this;
    }

    DynamicRendering(const DynamicRendering &o) { *this = o; }

    void CopyAttachmentNexts();

    bool active = false;
    bool suspended = false;
    VkRenderingFlags flags = 0;
    uint32_t layerCount = 0;
    uint32_t viewMask = 0;
    nbdarray<VkRenderingAttachmentInfo> color = {};
    VkRenderingAttachmentInfo depth = {};
    VkRenderingAttachmentInfo stencil = {};

    VkImageView fragmentDensityView = VK_NULL_HANDLE;
    VkImageLayout fragmentDensityLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageView shadingRateView = VK_NULL_HANDLE;
    VkImageLayout shadingRateLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkExtent2D shadingRateTexelSize = {1, 1};

    bool tileOnlyMSAAEnable = false;
    VkSampleCountFlagBits tileOnlyMSAASampleCount = VK_SAMPLE_COUNT_1_BIT;

    // VK_KHR_dynamic_rendering_local_read
    DynamicRenderingLocalRead localRead;

    // VK_EXT_custom_resolve
    bool beginCustomResolve = false;

  private:
    // VK_KHR_unified_image_layouts
    nbdarray<VkAttachmentFeedbackLoopInfoEXT> feedbacks;

    void CopyAttachmentNext(VkRenderingAttachmentInfo &info);
  } dynamicRendering;

  // fdm offset
  nbdarray<VkOffset2D> fragmentDensityMapOffsets;

  // shading rate
  VkExtent2D pipelineShadingRate = {1, 1};
  VkFragmentShadingRateCombinerOpKHR shadingRateCombiners[2] = {
      VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
      VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,
  };

  // shader objects
  ResourceId shaderObjects[NumShaderStages];

  // attachment feedback loop
  VkImageAspectFlags feedbackAspects = VK_IMAGE_ASPECT_NONE;

private:
  ResourceId renderPass;
  ResourceId framebuffer;
  nbdarray<ResourceId> fbattachments;

  void BindDescriptorSetsForPipeline(WrappedVulkan *vk, VkCommandBuffer cmd,
                                     VulkanStatePipeline &pipe, VkPipelineBindPoint bindPoint);

  void BindDescriptorSetsWithoutPipeline(WrappedVulkan *vk, VkCommandBuffer cmd,
                                         VulkanStatePipeline &pipe, VkPipelineBindPoint bindPoint);

  void BindDescriptorSetsForShaders(WrappedVulkan *vk, VkCommandBuffer cmd,
                                    VulkanStatePipeline &pipe, VkPipelineBindPoint bindPoint);

  void BindDescriptorSet(WrappedVulkan *vk, const DescSetLayout &descLayout, VkCommandBuffer cmd,
                         VkPipelineBindPoint bindPoint, uint32_t setIndex, uint32_t *dynamicOffsets);

  void BindLastPushConstants(WrappedVulkan *vk, VkCommandBuffer cmd);
};
