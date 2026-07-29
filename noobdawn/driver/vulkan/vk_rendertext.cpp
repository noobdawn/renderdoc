/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2018-2026 Baldur Karlsson
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

#include "vk_rendertext.h"
#include "maths/matrix.h"
#include "stb/stb_truetype.h"
#include "strings/string_utils.h"
#include "vk_shader_cache.h"

#define VULKAN 1
#include "data/glsl/glsl_ubos_cpp.h"

namespace
{
const nbdarray<VkFormat> BBFormats = {
    VK_FORMAT_R8G8B8A8_SRGB,
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_B8G8R8A8_SRGB,
    VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_A2B10G10R10_UNORM_PACK32,
    VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_R5G6B5_UNORM_PACK16,
};
};

VulkanTextRenderer::VulkanTextRenderer(WrappedVulkan *driver)
{
  m_Device = driver->GetDev();

  VkDevice dev = m_Device;

  VkResult vkr = VK_SUCCESS;

  VulkanShaderCache *shaderCache = driver->GetShaderCache();

  // create linear sampler
  VkSamplerCreateInfo sampInfo = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};

  sampInfo.flags = driver->DefaultSamplerCreateFlags();
  sampInfo.minFilter = sampInfo.magFilter = VK_FILTER_LINEAR;
  sampInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sampInfo.addressModeU = sampInfo.addressModeV = sampInfo.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampInfo.maxLod = 128.0f;

  vkr = ObjDisp(dev)->CreateSampler(Unwrap(dev), &sampInfo, NULL, &m_LinearSampler);
  NBDASSERTEQUAL(vkr, VK_SUCCESS);

  NameUnwrappedVulkanObject(m_LinearSampler, "m_LinearSampler");

  // just need enough for text rendering
  VkDescriptorPoolSize captureDescPoolTypes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
  };

  VkDescriptorPoolCreateInfo descpoolInfo = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      NULL,
      0,
      2,
      ARRAY_COUNT(captureDescPoolTypes),
      &captureDescPoolTypes[0],
  };

  // create descriptor pool
  vkr = ObjDisp(dev)->CreateDescriptorPool(Unwrap(dev), &descpoolInfo, NULL, &m_DescriptorPool);
  NBDASSERTEQUAL(vkr, VK_SUCCESS);

  // declare some common creation info structs
  VkPipelineLayoutCreateInfo pipeLayoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeLayoutInfo.setLayoutCount = 1;

  VkDescriptorSetAllocateInfo descSetAllocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                  NULL, m_DescriptorPool, 1, NULL};

  // declare the pipeline creation info and all of its sub-structures
  // these are modified as appropriate for each pipeline we create
  VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT,
       Unwrap(shaderCache->GetBuiltinModule(BuiltinShader::TextVS)), "main", NULL},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
       Unwrap(shaderCache->GetBuiltinModule(BuiltinShader::TextFS)), "main", NULL},
  };

  VkPipelineVertexInputStateCreateInfo vi = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };

  VkPipelineInputAssemblyStateCreateInfo ia = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
  };

  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkRect2D scissor = {{0, 0}, {16384, 16384}};

  VkPipelineViewportStateCreateInfo vp = {
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, &scissor};

  VkPipelineRasterizationStateCreateInfo rs = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
  };
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo msaa = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
  };
  msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState attState = {
      true,
      VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_OP_ADD,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_OP_ADD,
      0xf,
  };

  VkPipelineColorBlendStateCreateInfo cb = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      NULL,
      0,
      false,
      VK_LOGIC_OP_NO_OP,
      1,
      &attState,
      {1.0f, 1.0f, 1.0f, 1.0f},
  };

  VkDynamicState dynstates[] = {VK_DYNAMIC_STATE_VIEWPORT};

  VkPipelineDynamicStateCreateInfo dyn = {
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      NULL,
      0,
      ARRAY_COUNT(dynstates),
      dynstates,
  };

  VkGraphicsPipelineCreateInfo pipeInfo = {
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      NULL,
      0,
      2,
      stages,
      &vi,
      &ia,
      NULL,    // tess
      &vp,
      &rs,
      &msaa,
      NULL,
      &cb,
      &dyn,
      VK_NULL_HANDLE,
      VK_NULL_HANDLE,
      0,                 // sub pass
      VK_NULL_HANDLE,    // base pipeline handle
      -1,                // base pipeline index
  };

  VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};

  VkDescriptorSetLayoutBinding layoutBinding[] = {
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
      {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
      {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
  };

  VkDescriptorSetLayoutCreateInfo descsetLayoutInfo = {
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      NULL,
      0,
      ARRAY_COUNT(layoutBinding),
      &layoutBinding[0],
  };

  vkr = ObjDisp(dev)->CreateDescriptorSetLayout(Unwrap(dev), &descsetLayoutInfo, NULL,
                                                &m_TextDescSetLayout);
  NBDASSERTEQUAL(vkr, VK_SUCCESS);

  pipeLayoutInfo.pSetLayouts = &m_TextDescSetLayout;

  vkr = ObjDisp(dev)->CreatePipelineLayout(Unwrap(dev), &pipeLayoutInfo, NULL, &m_TextPipeLayout);
  NBDASSERTEQUAL(vkr, VK_SUCCESS);

  descSetAllocInfo.pSetLayouts = &m_TextDescSetLayout;
  vkr = ObjDisp(dev)->AllocateDescriptorSets(Unwrap(dev), &descSetAllocInfo, &m_TextDescSet);
  NBDASSERTEQUAL(vkr, VK_SUCCESS);

  // make the ring conservatively large to handle many lines of text * several frames
  m_TextGeneralUBO.Create(driver, dev, 128, 100, 0);
  m_TextGeneralUBO.Name("m_TextGeneralUBO");
  NBDCOMPILE_ASSERT(sizeof(FontUBOData) <= 128, "font uniforms size");

  // we only use a subset of the [MAX_SINGLE_LINE_LENGTH] array needed for each line, so this ring
  // can be smaller
  m_TextStringUBO.Create(driver, dev, 4096, 20, 0);
  m_TextGeneralUBO.Name("m_TextStringUBO");
  NBDCOMPILE_ASSERT(sizeof(StringUBOData) <= 4096, "font uniforms size");

  pipeInfo.layout = m_TextPipeLayout;

  {
    VkAttachmentDescription attDesc = {0,
                                       VK_FORMAT_R8G8B8A8_SRGB,
                                       VK_SAMPLE_COUNT_1_BIT,
                                       VK_ATTACHMENT_LOAD_OP_LOAD,
                                       VK_ATTACHMENT_STORE_OP_STORE,
                                       VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                       VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkAttachmentReference attRef = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription sub = {0};

    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &attRef;

    VkRenderPassCreateInfo rpinfo = {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &attDesc, 1, &sub,
    };

    NBDASSERTEQUAL(BBFormats.size(), int(NUM_BB_FORMATS));

    for(size_t i = 0; i < BBFormats.size(); i++)
    {
      VkRenderPass rp;
      attDesc.format = BBFormats[i];
      ObjDisp(dev)->CreateRenderPass(Unwrap(dev), &rpinfo, NULL, &rp);

      pipeInfo.renderPass = rp;
      vkr = ObjDisp(dev)->CreateGraphicsPipelines(Unwrap(dev), VK_NULL_HANDLE, 1, &pipeInfo, NULL,
                                                  &m_TextPipeline[i]);
      NBDASSERTEQUAL(vkr, VK_SUCCESS);

      ObjDisp(dev)->DestroyRenderPass(Unwrap(dev), rp, NULL);
    }
  }

  // create the actual font texture data and glyph data, for upload
  {
    const uint32_t width = FONT_TEX_WIDTH, height = FONT_TEX_HEIGHT;

    VkImageCreateInfo imInfo = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        NULL,
        driver->DefaultImageCreateFlags(),
        VK_IMAGE_TYPE_2D,
        VK_FORMAT_R8_UNORM,
        {width, height, 1},
        1,
        1,
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };

    nbdstr font = GetEmbeddedResource(sourcecodepro_ttf);
    byte *ttfdata = (byte *)font.c_str();

    const int firstChar = FONT_FIRST_CHAR;
    const int lastChar = FONT_LAST_CHAR;
    const int numChars = lastChar - firstChar + 1;

    NBDCOMPILE_ASSERT(FONT_FIRST_CHAR == int(' '), "Font defines are messed up");

    byte *buf = new byte[width * height];

    const float pixelHeight = 20.0f;

    stbtt_bakedchar chardata[numChars];
    stbtt_BakeFontBitmap(ttfdata, 0, pixelHeight, buf, width, height, firstChar, numChars, chardata);

    m_FontCharSize = pixelHeight;
#if ENABLED(RDOC_ANDROID)
    m_FontCharSize *= 2.0f;
#endif

    m_FontCharAspect = chardata[0].xadvance / pixelHeight;

    stbtt_fontinfo f = {0};
    stbtt_InitFont(&f, ttfdata, 0);

    int ascent = 0;
    stbtt_GetFontVMetrics(&f, &ascent, NULL, NULL);

    float maxheight = float(ascent) * stbtt_ScaleForPixelHeight(&f, pixelHeight);

    // create and fill image
    {
      vkr = ObjDisp(dev)->CreateImage(Unwrap(dev), &imInfo, NULL, &m_TextAtlas);
      NBDASSERTEQUAL(vkr, VK_SUCCESS);

      NameUnwrappedVulkanObject(m_TextAtlas, "m_TextAtlas");

      VkMemoryRequirements mrq = {0};
      ObjDisp(dev)->GetImageMemoryRequirements(Unwrap(dev), m_TextAtlas, &mrq);

      // allocate readback memory
      VkMemoryAllocateInfo allocInfo = {
          VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
          NULL,
          mrq.size,
          driver->GetGPULocalMemoryIndex(mrq.memoryTypeBits),
      };

      VkMemoryAllocateFlagsInfo memFlags = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
      if(driver->DescriptorBuffers())
      {
        allocInfo.pNext = &memFlags;
        memFlags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT |
                         VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT;
      }

      vkr = ObjDisp(dev)->AllocateMemory(Unwrap(dev), &allocInfo, NULL, &m_TextAtlasMem);
      NBDASSERTEQUAL(vkr, VK_SUCCESS);

      vkr = ObjDisp(dev)->BindImageMemory(Unwrap(dev), m_TextAtlas, m_TextAtlasMem, 0);
      NBDASSERTEQUAL(vkr, VK_SUCCESS);

      VkImageViewCreateInfo viewInfo = {
          VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          NULL,
          driver->DefaultImageViewCreateFlags(),
          m_TextAtlas,
          VK_IMAGE_VIEW_TYPE_2D,
          imInfo.format,
          {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };

      vkr = ObjDisp(dev)->CreateImageView(Unwrap(dev), &viewInfo, NULL, &m_TextAtlasView);
      NBDASSERTEQUAL(vkr, VK_SUCCESS);

      // create temporary memory and buffer to upload atlas
      // doesn't need to be ring'd, as it's static
      m_TextAtlasUpload.Create(driver, dev, 32768, 1, 0);
      m_TextAtlasUpload.Name("m_TextAtlasUpload");
      NBDCOMPILE_ASSERT(width * height <= 32768, "font uniform size");

      byte *pData = (byte *)m_TextAtlasUpload.Map();
      NBDASSERT(pData);

      memcpy(pData, buf, width * height);

      m_TextAtlasUpload.Unmap();
    }

    // doesn't need to be ring'd, as it's static
    m_TextGlyphUBO.Create(driver, dev, 4096, 1, 0);
    m_TextGlyphUBO.Name("m_TextGlyphUBO");
    NBDCOMPILE_ASSERT(sizeof(Vec4f) * 2 * (numChars + 1) < 4096, "font uniform size");

    FontGlyphData *glyphData = (FontGlyphData *)m_TextGlyphUBO.Map();

    glyphData[0].posdata = Vec4f();
    glyphData[0].uvdata = Vec4f();

    for(int i = 1; i < numChars; i++)
    {
      stbtt_bakedchar *b = chardata + i;

      float x = b->xoff;
      float y = b->yoff + maxheight;

      glyphData[i].posdata =
          Vec4f(x / b->xadvance, y / pixelHeight, b->xadvance / float(b->x1 - b->x0),
                pixelHeight / float(b->y1 - b->y0));
      glyphData[i].uvdata = Vec4f(b->x0, b->y0, b->x1, b->y1);
    }

    m_TextGlyphUBO.Unmap();
  }

  // perform GPU copy from m_TextAtlasUpload to m_TextAtlas with appropriate barriers
  {
    VkCommandBuffer textAtlasUploadCmd = driver->GetNextCmd();

    vkr = ObjDisp(textAtlasUploadCmd)->BeginCommandBuffer(Unwrap(textAtlasUploadCmd), &beginInfo);
    NBDASSERTEQUAL(vkr, VK_SUCCESS);

    // need to update image layout into valid state first
    VkImageMemoryBarrier copysrcbarrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        NULL,
        0,
        VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        m_TextAtlas,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };

    DoPipelineBarrier(textAtlasUploadCmd, 1, &copysrcbarrier);

    VkBufferMemoryBarrier uploadbarrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        NULL,
        VK_ACCESS_HOST_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        m_TextAtlasUpload.UnwrappedBuffer(),
        0,
        m_TextAtlasUpload.TotalSize(),
    };

    // ensure host writes finish before copy
    DoPipelineBarrier(textAtlasUploadCmd, 1, &uploadbarrier);

    VkBufferImageCopy bufRegion = {
        0,
        0,
        0,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        {
            0,
            0,
            0,
        },
        {FONT_TEX_WIDTH, FONT_TEX_HEIGHT, 1},
    };

    // copy to image
    ObjDisp(textAtlasUploadCmd)
        ->CmdCopyBufferToImage(Unwrap(textAtlasUploadCmd), m_TextAtlasUpload.UnwrappedBuffer(),
                               m_TextAtlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bufRegion);

    VkImageMemoryBarrier copydonebarrier = {
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        NULL,
        copysrcbarrier.dstAccessMask,
        VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        m_TextAtlas,
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };

    // ensure atlas is filled before reading in shader
    DoPipelineBarrier(textAtlasUploadCmd, 1, &copydonebarrier);

    ObjDisp(textAtlasUploadCmd)->EndCommandBuffer(Unwrap(textAtlasUploadCmd));
  }

  VkDescriptorBufferInfo bufInfo[3];
  NBDEraseEl(bufInfo);

  m_TextGeneralUBO.FillDescriptor(bufInfo[0]);
  m_TextGlyphUBO.FillDescriptor(bufInfo[1]);
  m_TextStringUBO.FillDescriptor(bufInfo[2]);

  VkDescriptorImageInfo atlasImInfo;
  atlasImInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  atlasImInfo.imageView = m_TextAtlasView;
  atlasImInfo.sampler = m_LinearSampler;

  VkWriteDescriptorSet textSetWrites[] = {
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, m_TextDescSet, 0, 0, 1,
       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &bufInfo[0], NULL},
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, m_TextDescSet, 1, 0, 1,
       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bufInfo[1], NULL},
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, m_TextDescSet, 2, 0, 1,
       VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NULL, &bufInfo[2], NULL},
      {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, m_TextDescSet, 3, 0, 1,
       VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &atlasImInfo, NULL, NULL},
  };

  ObjDisp(dev)->UpdateDescriptorSets(Unwrap(dev), ARRAY_COUNT(textSetWrites), textSetWrites, 0, NULL);
}

VulkanTextRenderer::~VulkanTextRenderer()
{
  VkDevice dev = m_Device;

  ObjDisp(dev)->DestroyDescriptorPool(Unwrap(dev), m_DescriptorPool, NULL);

  ObjDisp(dev)->DestroySampler(Unwrap(dev), m_LinearSampler, NULL);

  ObjDisp(dev)->DestroyDescriptorSetLayout(Unwrap(dev), m_TextDescSetLayout, NULL);
  ObjDisp(dev)->DestroyPipelineLayout(Unwrap(dev), m_TextPipeLayout, NULL);
  for(size_t i = 0; i < ARRAY_COUNT(m_TextPipeline); i++)
    ObjDisp(dev)->DestroyPipeline(Unwrap(dev), m_TextPipeline[i], NULL);

  ObjDisp(dev)->DestroyImageView(Unwrap(dev), m_TextAtlasView, NULL);
  ObjDisp(dev)->DestroyImage(Unwrap(dev), m_TextAtlas, NULL);
  ObjDisp(dev)->FreeMemory(Unwrap(dev), m_TextAtlasMem, NULL);

  m_TextGeneralUBO.Destroy();
  m_TextGlyphUBO.Destroy();
  m_TextStringUBO.Destroy();
  m_TextAtlasUpload.Destroy();
}

void VulkanTextRenderer::BeginText(const TextPrintState &textstate)
{
  VkClearValue clearval = {};
  VkRenderPassBeginInfo rpbegin = {
      VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      NULL,
      Unwrap(textstate.rp),
      Unwrap(textstate.fb),
      {{
           0,
           0,
       },
       {textstate.w, textstate.h}},
      1,
      &clearval,
  };
  ObjDisp(textstate.cmd)->CmdBeginRenderPass(Unwrap(textstate.cmd), &rpbegin, VK_SUBPASS_CONTENTS_INLINE);

  // assuming VK_FORMAT_R8G8B8A8_SRGB as default

  VkPipeline pipe = m_TextPipeline[0];

  int idx = BBFormats.indexOf(textstate.fmt);
  if(idx >= 0)
    pipe = m_TextPipeline[idx];
  else
    NBDERR("Unexpected backbuffer format %s", ToStr(textstate.fmt).c_str());

  ObjDisp(textstate.cmd)->CmdBindPipeline(Unwrap(textstate.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

  VkViewport viewport = {0.0f, 0.0f, (float)textstate.w, (float)textstate.h, 0.0f, 1.0f};
  ObjDisp(textstate.cmd)->CmdSetViewport(Unwrap(textstate.cmd), 0, 1, &viewport);
}

void VulkanTextRenderer::RenderText(const TextPrintState &textstate, float x, float y,
                                    const nbdstr &text)
{
  nbdarray<nbdstr> lines;
  split(text, lines, '\n');

  for(const nbdstr &line : lines)
  {
    RenderTextInternal(textstate, x, y, line);
    y += 1.0f;
  }
}

void VulkanTextRenderer::RenderTextInternal(const TextPrintState &textstate, float x, float y,
                                            const nbdstr &text)
{
  if(text.empty())
    return;

  uint32_t offsets[2] = {0};

  FontUBOData *ubo = (FontUBOData *)m_TextGeneralUBO.Map(&offsets[0]);

  ubo->TextPosition.x = x;
  ubo->TextPosition.y = y;

  ubo->FontScreenAspect.x = 1.0f / float(textstate.w);
  ubo->FontScreenAspect.y = 1.0f / float(textstate.h);

  ubo->TextSize = m_FontCharSize;
  ubo->FontScreenAspect.x *= m_FontCharAspect;

  ubo->CharacterSize.x = 1.0f / float(FONT_TEX_WIDTH);
  ubo->CharacterSize.y = 1.0f / float(FONT_TEX_HEIGHT);

  m_TextGeneralUBO.Unmap();

  size_t len = text.size();

  NBDASSERT(len <= MAX_SINGLE_LINE_LENGTH);

  // only map enough for our string
  StringUBOData *stringData = (StringUBOData *)m_TextStringUBO.Map(&offsets[1], len * sizeof(Vec4u));

  for(size_t i = 0; i < len; i++)
    stringData->chars[i].x = uint32_t(text[i] - ' ');

  m_TextStringUBO.Unmap();

  ObjDisp(textstate.cmd)
      ->CmdBindDescriptorSets(Unwrap(textstate.cmd), VK_PIPELINE_BIND_POINT_GRAPHICS,
                              m_TextPipeLayout, 0, 1, &m_TextDescSet, 2, offsets);

  ObjDisp(textstate.cmd)->CmdDraw(Unwrap(textstate.cmd), 6 * (uint32_t)len, 1, 0, 0);
}

void VulkanTextRenderer::EndText(const TextPrintState &textstate)
{
  ObjDisp(textstate.cmd)->CmdEndRenderPass(Unwrap(textstate.cmd));
}
