//
// Copyright 2018 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// vk_cache_utils.h:
//    Contains the classes for the Pipeline State Object cache as well as the RenderPass cache.
//    Also contains the structures for the packed descriptions for the RenderPass and Pipeline.
//

#ifndef LIBANGLE_RENDERER_VULKAN_VK_CACHE_UTILS_H_
#define LIBANGLE_RENDERER_VULKAN_VK_CACHE_UTILS_H_

#include "common/Color.h"
#include "common/FixedVector.h"
#include "libANGLE/renderer/vulkan/vk_utils.h"

namespace rx
{

// Some descriptor set and pipeline layout constants.
//
// The set/binding assignment is done as following:
//
// - Set 0 contains the ANGLE driver uniforms at binding 0.  Note that driver uniforms are updated
//   only under rare circumstances, such as viewport or depth range change.  However, there is only
//   one binding in this set.  This set is placed before Set 1 containing transform feedback
//   buffers, so that switching between xfb and non-xfb programs doesn't require rebinding this set.
//   Otherwise, as the layout of Set 1 changes (due to addition and removal of xfb buffers), and all
//   subsequent sets need to be rebound (due to Vulkan pipeline layout validation rules), we would
//   have needed to invalidateGraphicsDriverUniforms().
// - Set 1 contains uniform blocks created to encompass default uniforms.  1 binding is used per
//   pipeline stage.  Additionally, transform feedback buffers are bound from binding 2 and up.
// - Set 2 contains all textures (including texture buffers).
// - Set 3 contains all other shader resources, such as uniform and storage blocks, atomic counter
//   buffers, images and image buffers.

enum class DescriptorSetIndex : uint32_t
{
    Internal,        // ANGLE driver uniforms or internal shaders
    UniformsAndXfb,  // Uniforms set index
    Texture,         // Textures set index
    ShaderResource,  // Other shader resources set index

    InvalidEnum,
    EnumCount = InvalidEnum,
};

namespace vk
{
class DynamicDescriptorPool;
class ImageHelper;
enum class ImageLayout;

using PipelineAndSerial = ObjectAndSerial<Pipeline>;

using RefCountedDescriptorSetLayout    = RefCounted<DescriptorSetLayout>;
using RefCountedPipelineLayout         = RefCounted<PipelineLayout>;
using RefCountedSamplerYcbcrConversion = RefCounted<SamplerYcbcrConversion>;

// Helper macro that casts to a bitfield type then verifies no bits were dropped.
#define SetBitField(lhs, rhs)                                                         \
    do                                                                                \
    {                                                                                 \
        auto ANGLE_LOCAL_VAR = rhs;                                                   \
        lhs = static_cast<typename std::decay<decltype(lhs)>::type>(ANGLE_LOCAL_VAR); \
        ASSERT(static_cast<decltype(ANGLE_LOCAL_VAR)>(lhs) == ANGLE_LOCAL_VAR);       \
    } while (0)

// Packed Vk resource descriptions.
// Most Vk types use many more bits than required to represent the underlying data.
// Since ANGLE wants to cache things like RenderPasses and Pipeline State Objects using
// hashing (and also needs to check equality) we can optimize these operations by
// using fewer bits. Hence the packed types.
//
// One implementation note: these types could potentially be improved by using even
// fewer bits. For example, boolean values could be represented by a single bit instead
// of a uint8_t. However at the current time there are concerns about the portability
// of bitfield operators, and complexity issues with using bit mask operations. This is
// something we will likely want to investigate as the Vulkan implementation progresses.
//
// Second implementation note: the struct packing is also a bit fragile, and some of the
// packing requirements depend on using alignas and field ordering to get the result of
// packing nicely into the desired space. This is something we could also potentially fix
// with a redesign to use bitfields or bit mask operations.

// Enable struct padding warnings for the code below since it is used in caches.
ANGLE_ENABLE_STRUCT_PADDING_WARNINGS

enum ResourceAccess
{
    Unused,
    ReadOnly,
    Write,
};

inline void UpdateAccess(ResourceAccess *oldAccess, ResourceAccess newAccess)
{
    if (newAccess > *oldAccess)
    {
        *oldAccess = newAccess;
    }
}

enum RenderPassStoreOp
{
    Store    = VK_ATTACHMENT_STORE_OP_STORE,
    DontCare = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    NoneQCOM,
};
// ConvertRenderPassStoreOpToVkStoreOp rely on the fact that only NoneQCOM is different from VK
// enums.
static_assert(RenderPassStoreOp::NoneQCOM == 2, "ConvertRenderPassStoreOpToVkStoreOp must updated");

inline VkAttachmentStoreOp ConvertRenderPassStoreOpToVkStoreOp(RenderPassStoreOp storeOp)
{
    return storeOp == RenderPassStoreOp::NoneQCOM ? VK_ATTACHMENT_STORE_OP_NONE_QCOM
                                                  : static_cast<VkAttachmentStoreOp>(storeOp);
}

// There can be a maximum of IMPLEMENTATION_MAX_DRAW_BUFFERS color and resolve attachments, plus one
// depth/stencil attachment and one depth/stencil resolve attachment.
constexpr size_t kMaxFramebufferAttachments = gl::IMPLEMENTATION_MAX_DRAW_BUFFERS * 2 + 2;
template <typename T>
using FramebufferAttachmentArray = std::array<T, kMaxFramebufferAttachments>;
template <typename T>
using FramebufferAttachmentsVector = angle::FixedVector<T, kMaxFramebufferAttachments>;
using FramebufferAttachmentMask    = angle::BitSet<kMaxFramebufferAttachments>;

constexpr size_t kMaxFramebufferNonResolveAttachments = gl::IMPLEMENTATION_MAX_DRAW_BUFFERS + 1;
template <typename T>
using FramebufferNonResolveAttachmentArray = std::array<T, kMaxFramebufferNonResolveAttachments>;
using FramebufferNonResolveAttachmentMask  = angle::BitSet16<kMaxFramebufferNonResolveAttachments>;

class alignas(4) RenderPassDesc final
{
  public:
    RenderPassDesc();
    ~RenderPassDesc();
    RenderPassDesc(const RenderPassDesc &other);
    RenderPassDesc &operator=(const RenderPassDesc &other);

    // Set format for an enabled GL color attachment.
    void packColorAttachment(size_t colorIndexGL, angle::FormatID formatID);
    // Mark a GL color attachment index as disabled.
    void packColorAttachmentGap(size_t colorIndexGL);
    // The caller must pack the depth/stencil attachment last, which is packed right after the color
    // attachments (including gaps), i.e. with an index starting from |colorAttachmentRange()|.
    void packDepthStencilAttachment(angle::FormatID angleFormatID);
    void updateDepthStencilAccess(ResourceAccess access);
    // Indicate that a color attachment should have a corresponding resolve attachment.
    void packColorResolveAttachment(size_t colorIndexGL);
    // Remove the resolve attachment.  Used when optimizing blit through resolve attachment to
    // temporarily pack a resolve attachment and then remove it.
    void removeColorResolveAttachment(size_t colorIndexGL);
    // Indicate that a color attachment should take its data from the resolve attachment initially.
    void packColorUnresolveAttachment(size_t colorIndexGL);
    void removeColorUnresolveAttachment(size_t colorIndexGL);
    // Indicate that a depth/stencil attachment should have a corresponding resolve attachment.
    void packDepthStencilResolveAttachment();
    // Indicate that a depth/stencil attachment should take its data from the resolve attachment
    // initially.
    void packDepthStencilUnresolveAttachment(bool unresolveDepth, bool unresolveStencil);
    void removeDepthStencilUnresolveAttachment();

    void setWriteControlMode(gl::SrgbWriteControlMode mode);

    size_t hash() const;

    // Color attachments are in [0, colorAttachmentRange()), with possible gaps.
    size_t colorAttachmentRange() const { return mColorAttachmentRange; }
    size_t depthStencilAttachmentIndex() const { return colorAttachmentRange(); }

    bool isColorAttachmentEnabled(size_t colorIndexGL) const;
    bool hasDepthStencilAttachment() const;
    bool hasColorResolveAttachment(size_t colorIndexGL) const
    {
        return mColorResolveAttachmentMask.test(colorIndexGL);
    }
    gl::DrawBufferMask getColorUnresolveAttachmentMask() const
    {
        return mColorUnresolveAttachmentMask;
    }
    bool hasColorUnresolveAttachment(size_t colorIndexGL) const
    {
        return mColorUnresolveAttachmentMask.test(colorIndexGL);
    }
    bool hasDepthStencilResolveAttachment() const
    {
        return (mAttachmentFormats.back() & kResolveDepthStencilFlag) != 0;
    }
    bool hasDepthStencilUnresolveAttachment() const
    {
        return (mAttachmentFormats.back() & (kUnresolveDepthFlag | kUnresolveStencilFlag)) != 0;
    }
    bool hasDepthUnresolveAttachment() const
    {
        return (mAttachmentFormats.back() & kUnresolveDepthFlag) != 0;
    }
    bool hasStencilUnresolveAttachment() const
    {
        return (mAttachmentFormats.back() & kUnresolveStencilFlag) != 0;
    }
    gl::SrgbWriteControlMode getSRGBWriteControlMode() const
    {
        return ((mAttachmentFormats.back() & kSrgbWriteControlFlag) != 0)
                   ? gl::SrgbWriteControlMode::Linear
                   : gl::SrgbWriteControlMode::Default;
    }

    // Get the number of attachments in the Vulkan render pass, i.e. after removing disabled
    // color attachments.
    size_t attachmentCount() const;

    void setSamples(GLint samples);

    uint8_t samples() const { return 1u << mLogSamples; }

    void setFramebufferFetchMode(bool hasFramebufferFetch);
    bool getFramebufferFetchMode() const { return mHasFramebufferFetch; }

    void updateRenderToTexture(bool isRenderToTexture);
    bool isRenderToTexture() const { return (mAttachmentFormats.back() & kIsRenderToTexture) != 0; }

    angle::FormatID operator[](size_t index) const
    {
        ASSERT(index < gl::IMPLEMENTATION_MAX_DRAW_BUFFERS + 1);

        uint8_t format = mAttachmentFormats[index];
        if (index >= depthStencilAttachmentIndex())
        {
            format &= kDepthStencilFormatStorageMask;
        }
        return static_cast<angle::FormatID>(format);
    }

  private:
    // Store log(samples), to be able to store it in 3 bits.
    uint8_t mLogSamples : 3;
    uint8_t mColorAttachmentRange : 4;
    uint8_t mHasFramebufferFetch : 1;

    // Whether each color attachment has a corresponding resolve attachment.  Color resolve
    // attachments can be used to optimize resolve through glBlitFramebuffer() as well as support
    // GL_EXT_multisampled_render_to_texture and GL_EXT_multisampled_render_to_texture2.
    gl::DrawBufferMask mColorResolveAttachmentMask;

    // Whether each color attachment with a corresponding resolve attachment should be initialized
    // with said resolve attachment in an initial subpass.  This is an optimization to avoid
    // loadOp=LOAD on the implicit multisampled image used with multisampled-render-to-texture
    // render targets.  This operation is referred to as "unresolve".
    //
    // Unused when VK_EXT_multisampled_render_to_single_sampled is available.
    gl::DrawBufferMask mColorUnresolveAttachmentMask;

    // Color attachment formats are stored with their GL attachment indices.  The depth/stencil
    // attachment formats follow the last enabled color attachment.  When creating a render pass,
    // the disabled attachments are removed and the resulting attachments are packed.
    //
    // The attachment indices provided as input to various functions in this file are thus GL
    // attachment indices.  These indices are marked as such, e.g. colorIndexGL.  The render pass
    // (and corresponding framebuffer object) lists the packed attachments, with the corresponding
    // indices marked with Vk, e.g. colorIndexVk.  The subpass attachment references create the
    // link between the two index spaces.  The subpass declares attachment references with GL
    // indices (which corresponds to the location decoration of shader outputs).  The attachment
    // references then contain the Vulkan indices or VK_ATTACHMENT_UNUSED.
    //
    // For example, if GL uses color attachments 0 and 3, then there are two render pass
    // attachments (indexed 0 and 1) and 4 subpass attachments:
    //
    //  - Subpass attachment 0 -> Renderpass attachment 0
    //  - Subpass attachment 1 -> VK_ATTACHMENT_UNUSED
    //  - Subpass attachment 2 -> VK_ATTACHMENT_UNUSED
    //  - Subpass attachment 3 -> Renderpass attachment 1
    //
    // The resolve attachments are packed after the non-resolve attachments.  They use the same
    // formats, so they are not specified in this array.
    //
    // The depth/stencil angle::FormatID values are in the range [1, 7], and therefore require only
    // 3 bits to be stored.  As a result, the upper 5 bits of mAttachmentFormats.back() is free to
    // use for other purposes.
    FramebufferNonResolveAttachmentArray<uint8_t> mAttachmentFormats;

    // Depth/stencil format is stored in 3 bits.
    static constexpr uint8_t kDepthStencilFormatStorageMask = 0x7;

    // Flags stored in the upper 5 bits of mAttachmentFormats.back().
    static constexpr uint8_t kIsRenderToTexture       = 0x80;
    static constexpr uint8_t kResolveDepthStencilFlag = 0x40;
    static constexpr uint8_t kUnresolveDepthFlag      = 0x20;
    static constexpr uint8_t kUnresolveStencilFlag    = 0x10;
    static constexpr uint8_t kSrgbWriteControlFlag    = 0x08;
};

bool operator==(const RenderPassDesc &lhs, const RenderPassDesc &rhs);

constexpr size_t kRenderPassDescSize = sizeof(RenderPassDesc);
static_assert(kRenderPassDescSize == 12, "Size check failed");

struct PackedAttachmentOpsDesc final
{
    // VkAttachmentLoadOp is in range [0, 2], and VkAttachmentStoreOp is in range [0, 2].
    uint16_t loadOp : 2;
    uint16_t storeOp : 2;
    uint16_t stencilLoadOp : 2;
    uint16_t stencilStoreOp : 2;
    // If a corresponding resolve attachment exists, storeOp may already be DONT_CARE, and it's
    // unclear whether the attachment was invalidated or not.  This information is passed along here
    // so that the resolve attachment's storeOp can be set to DONT_CARE if the attachment is
    // invalidated, and if possible removed from the list of resolve attachments altogether.  Note
    // that the latter may not be possible if the render pass has multiple subpasses due to Vulkan
    // render pass compatibility rules.
    uint16_t isInvalidated : 1;
    uint16_t isStencilInvalidated : 1;
    uint16_t padding1 : 6;

    // 4-bits to force pad the structure to exactly 2 bytes.  Note that we currently don't support
    // any of the extension layouts, whose values start at 1'000'000'000.
    uint16_t initialLayout : 4;
    uint16_t finalLayout : 4;
    uint16_t padding2 : 8;
};

static_assert(sizeof(PackedAttachmentOpsDesc) == 4, "Size check failed");

class PackedAttachmentIndex;

class AttachmentOpsArray final
{
  public:
    AttachmentOpsArray();
    ~AttachmentOpsArray();
    AttachmentOpsArray(const AttachmentOpsArray &other);
    AttachmentOpsArray &operator=(const AttachmentOpsArray &other);

    const PackedAttachmentOpsDesc &operator[](PackedAttachmentIndex index) const;
    PackedAttachmentOpsDesc &operator[](PackedAttachmentIndex index);

    // Initialize an attachment op with all load and store operations.
    void initWithLoadStore(PackedAttachmentIndex index,
                           ImageLayout initialLayout,
                           ImageLayout finalLayout);

    void setLayouts(PackedAttachmentIndex index,
                    ImageLayout initialLayout,
                    ImageLayout finalLayout);
    void setOps(PackedAttachmentIndex index, VkAttachmentLoadOp loadOp, RenderPassStoreOp storeOp);
    void setStencilOps(PackedAttachmentIndex index,
                       VkAttachmentLoadOp loadOp,
                       RenderPassStoreOp storeOp);

    void setClearOp(PackedAttachmentIndex index);
    void setClearStencilOp(PackedAttachmentIndex index);

    size_t hash() const;

  private:
    gl::AttachmentArray<PackedAttachmentOpsDesc> mOps;
};

bool operator==(const AttachmentOpsArray &lhs, const AttachmentOpsArray &rhs);

static_assert(sizeof(AttachmentOpsArray) == 40, "Size check failed");

struct PackedAttribDesc final
{
    uint8_t format;
    uint8_t divisor;

    // Desktop drivers support
    uint16_t offset : kAttributeOffsetMaxBits;

    uint16_t compressed : 1;

    // Although technically stride can be any value in ES 2.0, in practice supporting stride
    // greater than MAX_USHORT should not be that helpful. Note that stride limits are
    // introduced in ES 3.1.
    uint16_t stride;
};

constexpr size_t kPackedAttribDescSize = sizeof(PackedAttribDesc);
static_assert(kPackedAttribDescSize == 6, "Size mismatch");

struct VertexInputAttributes final
{
    PackedAttribDesc attribs[gl::MAX_VERTEX_ATTRIBS];
};

constexpr size_t kVertexInputAttributesSize = sizeof(VertexInputAttributes);
static_assert(kVertexInputAttributesSize == 96, "Size mismatch");

struct RasterizationStateBits final
{
    // Note: Currently only 2 subpasses possible, so there are 5 bits in subpass that can be
    // repurposed.
    uint32_t subpass : 6;
    uint32_t depthClampEnable : 1;
    uint32_t rasterizationDiscardEnable : 1;
    uint32_t polygonMode : 4;
    uint32_t cullMode : 4;
    uint32_t frontFace : 4;
    uint32_t depthBiasEnable : 1;
    uint32_t sampleShadingEnable : 1;
    uint32_t alphaToCoverageEnable : 1;
    uint32_t alphaToOneEnable : 1;
    uint32_t rasterizationSamples : 8;
};

constexpr size_t kRasterizationStateBitsSize = sizeof(RasterizationStateBits);
static_assert(kRasterizationStateBitsSize == 4, "Size check failed");

struct PackedRasterizationAndMultisampleStateInfo final
{
    RasterizationStateBits bits;
    // Padded to ensure there's no gaps in this structure or those that use it.
    float minSampleShading;
    uint32_t sampleMask[gl::MAX_SAMPLE_MASK_WORDS];
    // Note: depth bias clamp is only exposed in a 3.1 extension, but left here for completeness.
    float depthBiasClamp;
    float depthBiasConstantFactor;
    float depthBiasSlopeFactor;
    float lineWidth;
};

constexpr size_t kPackedRasterizationAndMultisampleStateSize =
    sizeof(PackedRasterizationAndMultisampleStateInfo);
static_assert(kPackedRasterizationAndMultisampleStateSize == 32, "Size check failed");

struct StencilOps final
{
    uint8_t fail : 4;
    uint8_t pass : 4;
    uint8_t depthFail : 4;
    uint8_t compare : 4;
};

constexpr size_t kStencilOpsSize = sizeof(StencilOps);
static_assert(kStencilOpsSize == 2, "Size check failed");

struct PackedStencilOpState final
{
    StencilOps ops;
    uint8_t compareMask;
    uint8_t writeMask;
};

constexpr size_t kPackedStencilOpSize = sizeof(PackedStencilOpState);
static_assert(kPackedStencilOpSize == 4, "Size check failed");

struct DepthStencilEnableFlags final
{
    uint8_t depthTest : 2;  // these only need one bit each. the extra is used as padding.
    uint8_t depthWrite : 2;
    uint8_t depthBoundsTest : 2;
    uint8_t stencilTest : 2;
};

constexpr size_t kDepthStencilEnableFlagsSize = sizeof(DepthStencilEnableFlags);
static_assert(kDepthStencilEnableFlagsSize == 1, "Size check failed");

// We are borrowing three bits here for surface rotation, even though it has nothing to do with
// depth stencil.
struct DepthCompareOpAndSurfaceRotation final
{
    uint8_t depthCompareOp : 4;
    uint8_t surfaceRotation : 3;
    uint8_t padding : 1;
};
constexpr size_t kDepthCompareOpAndSurfaceRotationSize = sizeof(DepthCompareOpAndSurfaceRotation);
static_assert(kDepthCompareOpAndSurfaceRotationSize == 1, "Size check failed");

struct PackedDepthStencilStateInfo final
{
    DepthStencilEnableFlags enable;
    uint8_t frontStencilReference;
    uint8_t backStencilReference;
    DepthCompareOpAndSurfaceRotation depthCompareOpAndSurfaceRotation;

    float minDepthBounds;
    float maxDepthBounds;
    PackedStencilOpState front;
    PackedStencilOpState back;
};

constexpr size_t kPackedDepthStencilStateSize = sizeof(PackedDepthStencilStateInfo);
static_assert(kPackedDepthStencilStateSize == 20, "Size check failed");
static_assert(static_cast<int>(SurfaceRotation::EnumCount) <= 8, "Size check failed");

struct LogicOpState final
{
    uint8_t opEnable : 1;
    uint8_t op : 7;
};

constexpr size_t kLogicOpStateSize = sizeof(LogicOpState);
static_assert(kLogicOpStateSize == 1, "Size check failed");

struct PackedColorBlendAttachmentState final
{
    uint16_t srcColorBlendFactor : 5;
    uint16_t dstColorBlendFactor : 5;
    uint16_t colorBlendOp : 6;
    uint16_t srcAlphaBlendFactor : 5;
    uint16_t dstAlphaBlendFactor : 5;
    uint16_t alphaBlendOp : 6;
};

constexpr size_t kPackedColorBlendAttachmentStateSize = sizeof(PackedColorBlendAttachmentState);
static_assert(kPackedColorBlendAttachmentStateSize == 4, "Size check failed");

struct PrimitiveState final
{
    uint16_t topology : 9;
    uint16_t patchVertices : 6;
    uint16_t restartEnable : 1;
};

constexpr size_t kPrimitiveStateSize = sizeof(PrimitiveState);
static_assert(kPrimitiveStateSize == 2, "Size check failed");

struct PackedInputAssemblyAndColorBlendStateInfo final
{
    uint8_t colorWriteMaskBits[gl::IMPLEMENTATION_MAX_DRAW_BUFFERS / 2];
    PackedColorBlendAttachmentState attachments[gl::IMPLEMENTATION_MAX_DRAW_BUFFERS];
    float blendConstants[4];
    LogicOpState logic;
    uint8_t blendEnableMask;
    PrimitiveState primitive;
};

struct PackedScissor final
{
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
};

struct PackedExtent final
{
    uint16_t width;
    uint16_t height;
};
// This is invalid value for PackedScissor.x. It is used to indicate scissor is a dynamic state
constexpr int32_t kDynamicScissorSentinel = std::numeric_limits<decltype(PackedScissor::x)>::max();

constexpr size_t kPackedInputAssemblyAndColorBlendStateSize =
    sizeof(PackedInputAssemblyAndColorBlendStateInfo);
static_assert(kPackedInputAssemblyAndColorBlendStateSize == 56, "Size check failed");

constexpr size_t kGraphicsPipelineDescSumOfSizes =
    kVertexInputAttributesSize + kRenderPassDescSize + kPackedRasterizationAndMultisampleStateSize +
    kPackedDepthStencilStateSize + kPackedInputAssemblyAndColorBlendStateSize + sizeof(VkViewport) +
    sizeof(PackedScissor) + sizeof(PackedExtent);

// Number of dirty bits in the dirty bit set.
constexpr size_t kGraphicsPipelineDirtyBitBytes = 4;
constexpr static size_t kNumGraphicsPipelineDirtyBits =
    kGraphicsPipelineDescSumOfSizes / kGraphicsPipelineDirtyBitBytes;
static_assert(kNumGraphicsPipelineDirtyBits <= 64, "Too many pipeline dirty bits");

// Set of dirty bits. Each bit represents kGraphicsPipelineDirtyBitBytes in the desc.
using GraphicsPipelineTransitionBits = angle::BitSet<kNumGraphicsPipelineDirtyBits>;

// State changes are applied through the update methods. Each update method can also have a
// sibling method that applies the update without marking a state transition. The non-transition
// update methods are used for internal shader pipelines. Not every non-transition update method
// is implemented yet as not every state is used in internal shaders.
class GraphicsPipelineDesc final
{
  public:
    // Use aligned allocation and free so we can use the alignas keyword.
    void *operator new(std::size_t size);
    void operator delete(void *ptr);

    GraphicsPipelineDesc();
    ~GraphicsPipelineDesc();
    GraphicsPipelineDesc(const GraphicsPipelineDesc &other);
    GraphicsPipelineDesc &operator=(const GraphicsPipelineDesc &other);

    size_t hash() const;
    bool operator==(const GraphicsPipelineDesc &other) const;

    void initDefaults(const ContextVk *contextVk);

    // For custom comparisons.
    template <typename T>
    const T *getPtr() const
    {
        return reinterpret_cast<const T *>(this);
    }

    angle::Result initializePipeline(ContextVk *contextVk,
                                     const PipelineCache &pipelineCacheVk,
                                     const RenderPass &compatibleRenderPass,
                                     const PipelineLayout &pipelineLayout,
                                     const gl::AttributesMask &activeAttribLocationsMask,
                                     const gl::ComponentTypeMask &programAttribsTypeMask,
                                     const ShaderModule *vertexModule,
                                     const ShaderModule *fragmentModule,
                                     const ShaderModule *geometryModule,
                                     const ShaderModule *tessControlModule,
                                     const ShaderModule *tessEvaluationModule,
                                     const SpecializationConstants &specConsts,
                                     Pipeline *pipelineOut) const;

    // Vertex input state. For ES 3.1 this should be separated into binding and attribute.
    void updateVertexInput(GraphicsPipelineTransitionBits *transition,
                           uint32_t attribIndex,
                           GLuint stride,
                           GLuint divisor,
                           angle::FormatID format,
                           bool compressed,
                           GLuint relativeOffset);

    // Input assembly info
    void updateTopology(GraphicsPipelineTransitionBits *transition, gl::PrimitiveMode drawMode);
    void updatePrimitiveRestartEnabled(GraphicsPipelineTransitionBits *transition,
                                       bool primitiveRestartEnabled);

    // Raster states
    void setCullMode(VkCullModeFlagBits cullMode);
    void updateCullMode(GraphicsPipelineTransitionBits *transition,
                        const gl::RasterizerState &rasterState);
    void updateFrontFace(GraphicsPipelineTransitionBits *transition,
                         const gl::RasterizerState &rasterState,
                         bool invertFrontFace);
    void updateLineWidth(GraphicsPipelineTransitionBits *transition, float lineWidth);
    void updateRasterizerDiscardEnabled(GraphicsPipelineTransitionBits *transition,
                                        bool rasterizerDiscardEnabled);

    // Multisample states
    uint32_t getRasterizationSamples() const;
    void setRasterizationSamples(uint32_t rasterizationSamples);
    void updateRasterizationSamples(GraphicsPipelineTransitionBits *transition,
                                    uint32_t rasterizationSamples);
    void updateAlphaToCoverageEnable(GraphicsPipelineTransitionBits *transition, bool enable);
    void updateAlphaToOneEnable(GraphicsPipelineTransitionBits *transition, bool enable);
    void updateSampleMask(GraphicsPipelineTransitionBits *transition,
                          uint32_t maskNumber,
                          uint32_t mask);

    void updateSampleShading(GraphicsPipelineTransitionBits *transition, bool enable, float value);

    // RenderPass description.
    const RenderPassDesc &getRenderPassDesc() const { return mRenderPassDesc; }

    void setRenderPassDesc(const RenderPassDesc &renderPassDesc);
    void updateRenderPassDesc(GraphicsPipelineTransitionBits *transition,
                              const RenderPassDesc &renderPassDesc);

    // Blend states
    void updateBlendEnabled(GraphicsPipelineTransitionBits *transition,
                            gl::DrawBufferMask blendEnabledMask);
    void updateBlendColor(GraphicsPipelineTransitionBits *transition, const gl::ColorF &color);
    void updateBlendFuncs(GraphicsPipelineTransitionBits *transition,
                          const gl::BlendStateExt &blendStateExt);
    void updateBlendEquations(GraphicsPipelineTransitionBits *transition,
                              const gl::BlendStateExt &blendStateExt);
    void setColorWriteMasks(gl::BlendStateExt::ColorMaskStorage::Type colorMasks,
                            const gl::DrawBufferMask &alphaMask,
                            const gl::DrawBufferMask &enabledDrawBuffers);
    void setSingleColorWriteMask(uint32_t colorIndexGL, VkColorComponentFlags colorComponentFlags);
    void updateColorWriteMasks(GraphicsPipelineTransitionBits *transition,
                               gl::BlendStateExt::ColorMaskStorage::Type colorMasks,
                               const gl::DrawBufferMask &alphaMask,
                               const gl::DrawBufferMask &enabledDrawBuffers);

    // Depth/stencil states.
    void setDepthTestEnabled(bool enabled);
    void setDepthWriteEnabled(bool enabled);
    void setDepthFunc(VkCompareOp op);
    void setDepthClampEnabled(bool enabled);
    void setStencilTestEnabled(bool enabled);
    void setStencilFrontFuncs(uint8_t reference, VkCompareOp compareOp, uint8_t compareMask);
    void setStencilBackFuncs(uint8_t reference, VkCompareOp compareOp, uint8_t compareMask);
    void setStencilFrontOps(VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp);
    void setStencilBackOps(VkStencilOp failOp, VkStencilOp passOp, VkStencilOp depthFailOp);
    void setStencilFrontWriteMask(uint8_t mask);
    void setStencilBackWriteMask(uint8_t mask);
    void updateDepthTestEnabled(GraphicsPipelineTransitionBits *transition,
                                const gl::DepthStencilState &depthStencilState,
                                const gl::Framebuffer *drawFramebuffer);
    void updateDepthFunc(GraphicsPipelineTransitionBits *transition,
                         const gl::DepthStencilState &depthStencilState);
    void updateDepthWriteEnabled(GraphicsPipelineTransitionBits *transition,
                                 const gl::DepthStencilState &depthStencilState,
                                 const gl::Framebuffer *drawFramebuffer);
    void updateStencilTestEnabled(GraphicsPipelineTransitionBits *transition,
                                  const gl::DepthStencilState &depthStencilState,
                                  const gl::Framebuffer *drawFramebuffer);
    void updateStencilFrontFuncs(GraphicsPipelineTransitionBits *transition,
                                 GLint ref,
                                 const gl::DepthStencilState &depthStencilState);
    void updateStencilBackFuncs(GraphicsPipelineTransitionBits *transition,
                                GLint ref,
                                const gl::DepthStencilState &depthStencilState);
    void updateStencilFrontOps(GraphicsPipelineTransitionBits *transition,
                               const gl::DepthStencilState &depthStencilState);
    void updateStencilBackOps(GraphicsPipelineTransitionBits *transition,
                              const gl::DepthStencilState &depthStencilState);
    void updateStencilFrontWriteMask(GraphicsPipelineTransitionBits *transition,
                                     const gl::DepthStencilState &depthStencilState,
                                     const gl::Framebuffer *drawFramebuffer);
    void updateStencilBackWriteMask(GraphicsPipelineTransitionBits *transition,
                                    const gl::DepthStencilState &depthStencilState,
                                    const gl::Framebuffer *drawFramebuffer);

    // Depth offset.
    void updatePolygonOffsetFillEnabled(GraphicsPipelineTransitionBits *transition, bool enabled);
    void updatePolygonOffset(GraphicsPipelineTransitionBits *transition,
                             const gl::RasterizerState &rasterState);

    // Viewport and scissor.
    void setViewport(const VkViewport &viewport);
    void updateViewport(GraphicsPipelineTransitionBits *transition, const VkViewport &viewport);
    void updateDepthRange(GraphicsPipelineTransitionBits *transition,
                          float nearPlane,
                          float farPlane);
    void setDynamicScissor();
    void setScissor(const VkRect2D &scissor);
    void updateScissor(GraphicsPipelineTransitionBits *transition, const VkRect2D &scissor);

    // Tessellation
    void updatePatchVertices(GraphicsPipelineTransitionBits *transition, GLuint value);

    // Subpass
    void resetSubpass(GraphicsPipelineTransitionBits *transition);
    void nextSubpass(GraphicsPipelineTransitionBits *transition);
    void setSubpass(uint32_t subpass);
    uint32_t getSubpass() const;

    void updateSurfaceRotation(GraphicsPipelineTransitionBits *transition,
                               const SurfaceRotation surfaceRotation);
    SurfaceRotation getSurfaceRotation() const
    {
        return static_cast<SurfaceRotation>(
            mDepthStencilStateInfo.depthCompareOpAndSurfaceRotation.surfaceRotation);
    }

    void updateDrawableSize(GraphicsPipelineTransitionBits *transition,
                            uint32_t width,
                            uint32_t height);
    const PackedExtent &getDrawableSize() const { return mDrawableSize; }

  private:
    void updateSubpass(GraphicsPipelineTransitionBits *transition, uint32_t subpass);

    VertexInputAttributes mVertexInputAttribs;
    RenderPassDesc mRenderPassDesc;
    PackedRasterizationAndMultisampleStateInfo mRasterizationAndMultisampleStateInfo;
    PackedDepthStencilStateInfo mDepthStencilStateInfo;
    PackedInputAssemblyAndColorBlendStateInfo mInputAssemblyAndColorBlendStateInfo;
    VkViewport mViewport;
    // The special value of .offset.x == INT_MIN for scissor implies dynamic scissor that needs to
    // be set through vkCmdSetScissor.
    PackedScissor mScissor;
    PackedExtent mDrawableSize;
};

// Verify the packed pipeline description has no gaps in the packing.
// This is not guaranteed by the spec, but is validated by a compile-time check.
// No gaps or padding at the end ensures that hashing and memcmp checks will not run
// into uninitialized memory regions.
constexpr size_t kGraphicsPipelineDescSize = sizeof(GraphicsPipelineDesc);
static_assert(kGraphicsPipelineDescSize == kGraphicsPipelineDescSumOfSizes, "Size mismatch");

constexpr uint32_t kMaxDescriptorSetLayoutBindings =
    std::max(gl::IMPLEMENTATION_MAX_ACTIVE_TEXTURES,
             gl::IMPLEMENTATION_MAX_UNIFORM_BUFFER_BINDINGS);

using DescriptorSetLayoutBindingVector =
    angle::FixedVector<VkDescriptorSetLayoutBinding, kMaxDescriptorSetLayoutBindings>;

// A packed description of a descriptor set layout. Use similarly to RenderPassDesc and
// GraphicsPipelineDesc. Currently we only need to differentiate layouts based on sampler and ubo
// usage. In the future we could generalize this.
class DescriptorSetLayoutDesc final
{
  public:
    DescriptorSetLayoutDesc();
    ~DescriptorSetLayoutDesc();
    DescriptorSetLayoutDesc(const DescriptorSetLayoutDesc &other);
    DescriptorSetLayoutDesc &operator=(const DescriptorSetLayoutDesc &other);

    size_t hash() const;
    bool operator==(const DescriptorSetLayoutDesc &other) const;

    void update(uint32_t bindingIndex,
                VkDescriptorType type,
                uint32_t count,
                VkShaderStageFlags stages,
                const Sampler *immutableSampler);

    void unpackBindings(DescriptorSetLayoutBindingVector *bindings,
                        std::vector<VkSampler> *immutableSamplers) const;

  private:
    // There is a small risk of an issue if the sampler cache is evicted but not the descriptor
    // cache we would have an invalid handle here. Thus propose follow-up work:
    // TODO: https://issuetracker.google.com/issues/159156775: Have immutable sampler use serial
    struct PackedDescriptorSetBinding
    {
        uint8_t type;    // Stores a packed VkDescriptorType descriptorType.
        uint8_t stages;  // Stores a packed VkShaderStageFlags.
        uint16_t count;  // Stores a packed uint32_t descriptorCount.
        uint32_t pad;
        VkSampler immutableSampler;
    };

    // 4x 32bit
    static_assert(sizeof(PackedDescriptorSetBinding) == 16, "Unexpected size");

    // This is a compact representation of a descriptor set layout.
    std::array<PackedDescriptorSetBinding, kMaxDescriptorSetLayoutBindings>
        mPackedDescriptorSetLayout;
};

// The following are for caching descriptor set layouts. Limited to max four descriptor set layouts.
// This can be extended in the future.
constexpr size_t kMaxDescriptorSetLayouts = 4;

struct PackedPushConstantRange
{
    uint32_t offset;
    uint32_t size;
};

template <typename T>
using DescriptorSetArray              = angle::PackedEnumMap<DescriptorSetIndex, T>;
using DescriptorSetLayoutPointerArray = DescriptorSetArray<BindingPointer<DescriptorSetLayout>>;
template <typename T>
using PushConstantRangeArray = gl::ShaderMap<T>;

class PipelineLayoutDesc final
{
  public:
    PipelineLayoutDesc();
    ~PipelineLayoutDesc();
    PipelineLayoutDesc(const PipelineLayoutDesc &other);
    PipelineLayoutDesc &operator=(const PipelineLayoutDesc &rhs);

    size_t hash() const;
    bool operator==(const PipelineLayoutDesc &other) const;

    void updateDescriptorSetLayout(DescriptorSetIndex setIndex,
                                   const DescriptorSetLayoutDesc &desc);
    void updatePushConstantRange(gl::ShaderType shaderType, uint32_t offset, uint32_t size);

    const PushConstantRangeArray<PackedPushConstantRange> &getPushConstantRanges() const;

  private:
    DescriptorSetArray<DescriptorSetLayoutDesc> mDescriptorSetLayouts;
    PushConstantRangeArray<PackedPushConstantRange> mPushConstantRanges;

    // Verify the arrays are properly packed.
    static_assert(sizeof(decltype(mDescriptorSetLayouts)) ==
                      (sizeof(DescriptorSetLayoutDesc) * kMaxDescriptorSetLayouts),
                  "Unexpected size");
    static_assert(sizeof(decltype(mPushConstantRanges)) ==
                      (sizeof(PackedPushConstantRange) * angle::EnumSize<gl::ShaderType>()),
                  "Unexpected size");
};

// Verify the structure is properly packed.
static_assert(sizeof(PipelineLayoutDesc) == (sizeof(DescriptorSetArray<DescriptorSetLayoutDesc>) +
                                             sizeof(gl::ShaderMap<PackedPushConstantRange>)),
              "Unexpected Size");

// Packed sampler description for the sampler cache.
class SamplerDesc final
{
  public:
    SamplerDesc();
    SamplerDesc(const angle::FeaturesVk &featuresVk,
                const gl::SamplerState &samplerState,
                bool stencilMode,
                uint64_t externalFormat);
    ~SamplerDesc();

    SamplerDesc(const SamplerDesc &other);
    SamplerDesc &operator=(const SamplerDesc &rhs);

    void update(const angle::FeaturesVk &featuresVk,
                const gl::SamplerState &samplerState,
                bool stencilMode,
                uint64_t externalFormat);
    void reset();
    angle::Result init(ContextVk *contextVk, Sampler *sampler) const;

    size_t hash() const;
    bool operator==(const SamplerDesc &other) const;

  private:
    // 32*4 bits for floating point data.
    // Note: anisotropy enabled is implicitly determined by maxAnisotropy and caps.
    float mMipLodBias;
    float mMaxAnisotropy;
    float mMinLod;
    float mMaxLod;

    // If the sampler needs to convert the image content (e.g. from YUV to RGB) then mExternalFormat
    // will be non-zero and match the external format as returned from
    // vkGetAndroidHardwareBufferPropertiesANDROID.
    // The externalFormat is guaranteed to be unique and any image with the same externalFormat can
    // use the same conversion sampler. Thus externalFormat works as a Serial() used elsewhere in
    // ANGLE.
    uint64_t mExternalFormat;

    // 16 bits for modes + states.
    // 1 bit per filter (only 2 possible values in GL: linear/nearest)
    uint16_t mMagFilter : 1;
    uint16_t mMinFilter : 1;
    uint16_t mMipmapMode : 1;

    // 3 bits per address mode (5 possible values)
    uint16_t mAddressModeU : 3;
    uint16_t mAddressModeV : 3;
    uint16_t mAddressModeW : 3;

    // 1 bit for compare enabled (2 possible values)
    uint16_t mCompareEnabled : 1;

    // 3 bits for compare op. (8 possible values)
    uint16_t mCompareOp : 3;

    // Border color and unnormalized coordinates implicitly set to contants.

    // 48 extra bits reserved for future use.
    uint16_t mReserved[3];
};

static_assert(sizeof(SamplerDesc) == 32, "Unexpected SamplerDesc size");

// Disable warnings about struct padding.
ANGLE_DISABLE_STRUCT_PADDING_WARNINGS

class PipelineHelper;

struct GraphicsPipelineTransition
{
    GraphicsPipelineTransition();
    GraphicsPipelineTransition(const GraphicsPipelineTransition &other);
    GraphicsPipelineTransition(GraphicsPipelineTransitionBits bits,
                               const GraphicsPipelineDesc *desc,
                               PipelineHelper *pipeline);

    GraphicsPipelineTransitionBits bits;
    const GraphicsPipelineDesc *desc;
    PipelineHelper *target;
};

ANGLE_INLINE GraphicsPipelineTransition::GraphicsPipelineTransition() = default;

ANGLE_INLINE GraphicsPipelineTransition::GraphicsPipelineTransition(
    const GraphicsPipelineTransition &other) = default;

ANGLE_INLINE GraphicsPipelineTransition::GraphicsPipelineTransition(
    GraphicsPipelineTransitionBits bits,
    const GraphicsPipelineDesc *desc,
    PipelineHelper *pipeline)
    : bits(bits), desc(desc), target(pipeline)
{}

ANGLE_INLINE bool GraphicsPipelineTransitionMatch(GraphicsPipelineTransitionBits bitsA,
                                                  GraphicsPipelineTransitionBits bitsB,
                                                  const GraphicsPipelineDesc &descA,
                                                  const GraphicsPipelineDesc &descB)
{
    if (bitsA != bitsB)
        return false;

    // We currently mask over 4 bytes of the pipeline description with each dirty bit.
    // We could consider using 8 bytes and a mask of 32 bits. This would make some parts
    // of the code faster. The for loop below would scan over twice as many bits per iteration.
    // But there may be more collisions between the same dirty bit masks leading to different
    // transitions. Thus there may be additional cost when applications use many transitions.
    // We should revisit this in the future and investigate using different bit widths.
    static_assert(sizeof(uint32_t) == kGraphicsPipelineDirtyBitBytes, "Size mismatch");

    const uint32_t *rawPtrA = descA.getPtr<uint32_t>();
    const uint32_t *rawPtrB = descB.getPtr<uint32_t>();

    for (size_t dirtyBit : bitsA)
    {
        if (rawPtrA[dirtyBit] != rawPtrB[dirtyBit])
            return false;
    }

    return true;
}

class PipelineHelper final : angle::NonCopyable
{
  public:
    PipelineHelper();
    ~PipelineHelper();
    inline explicit PipelineHelper(Pipeline &&pipeline);

    void destroy(VkDevice device);

    void updateSerial(Serial serial) { mSerial = serial; }
    bool valid() const { return mPipeline.valid(); }
    Serial getSerial() const { return mSerial; }
    Pipeline &getPipeline() { return mPipeline; }

    ANGLE_INLINE bool findTransition(GraphicsPipelineTransitionBits bits,
                                     const GraphicsPipelineDesc &desc,
                                     PipelineHelper **pipelineOut) const
    {
        // Search could be improved using sorting or hashing.
        for (const GraphicsPipelineTransition &transition : mTransitions)
        {
            if (GraphicsPipelineTransitionMatch(transition.bits, bits, *transition.desc, desc))
            {
                *pipelineOut = transition.target;
                return true;
            }
        }

        return false;
    }

    void addTransition(GraphicsPipelineTransitionBits bits,
                       const GraphicsPipelineDesc *desc,
                       PipelineHelper *pipeline);

  private:
    std::vector<GraphicsPipelineTransition> mTransitions;
    Serial mSerial;
    Pipeline mPipeline;
};

ANGLE_INLINE PipelineHelper::PipelineHelper(Pipeline &&pipeline) : mPipeline(std::move(pipeline)) {}

struct ImageSubresourceRange
{
    uint16_t level : 10;            // GL max is 1000 (fits in 10 bits).
    uint16_t levelCount : 6;        // Max 63 levels (2 ** 6 - 1). If we need more, take from layer.
    uint16_t layer : 13;            // Implementation max is 2048 (11 bits).
    uint16_t singleLayer : 1;       // true/false only. Not possible to use sub-slices of levels.
    uint16_t srgbDecodeMode : 1;    // Values from vk::SrgbDecodeMode.
    uint16_t srgbOverrideMode : 1;  // Values from gl::SrgbOverride, either Default or SRGB.
};

static_assert(sizeof(ImageSubresourceRange) == sizeof(uint32_t), "Size mismatch");

constexpr ImageSubresourceRange kInvalidImageSubresourceRange = {0, 0, 0, 0, 0, 0};

struct ImageOrBufferViewSubresourceSerial
{
    ImageOrBufferViewSerial viewSerial;
    ImageSubresourceRange subresource;
};

static_assert(sizeof(ImageOrBufferViewSubresourceSerial) == sizeof(uint64_t), "Size mismatch");

constexpr ImageOrBufferViewSubresourceSerial kInvalidImageOrBufferViewSubresourceSerial = {
    kInvalidImageOrBufferViewSerial, kInvalidImageSubresourceRange};

class TextureDescriptorDesc
{
  public:
    TextureDescriptorDesc();
    ~TextureDescriptorDesc();

    TextureDescriptorDesc(const TextureDescriptorDesc &other);
    TextureDescriptorDesc &operator=(const TextureDescriptorDesc &other);

    void update(size_t index,
                ImageOrBufferViewSubresourceSerial viewSerial,
                SamplerSerial samplerSerial);
    size_t hash() const;
    void reset();

    bool operator==(const TextureDescriptorDesc &other) const;

    // Note: this is an exclusive index. If there is one index it will return "1".
    uint32_t getMaxIndex() const { return mMaxIndex; }

  private:
    uint32_t mMaxIndex;

    ANGLE_ENABLE_STRUCT_PADDING_WARNINGS
    struct TexUnitSerials
    {
        ImageOrBufferViewSubresourceSerial view;
        SamplerSerial sampler;
    };
    gl::ActiveTextureArray<TexUnitSerials> mSerials;
    ANGLE_DISABLE_STRUCT_PADDING_WARNINGS
};

class UniformsAndXfbDescriptorDesc
{
  public:
    UniformsAndXfbDescriptorDesc();
    ~UniformsAndXfbDescriptorDesc();

    UniformsAndXfbDescriptorDesc(const UniformsAndXfbDescriptorDesc &other);
    UniformsAndXfbDescriptorDesc &operator=(const UniformsAndXfbDescriptorDesc &other);

    BufferSerial getDefaultUniformBufferSerial() const
    {
        return mBufferSerials[kDefaultUniformBufferIndex];
    }
    void updateDefaultUniformBuffer(BufferSerial bufferSerial)
    {
        mBufferSerials[kDefaultUniformBufferIndex] = bufferSerial;
        mBufferCount = std::max(mBufferCount, static_cast<uint32_t>(1));
    }
    void updateTransformFeedbackBuffer(size_t xfbIndex,
                                       BufferSerial bufferSerial,
                                       VkDeviceSize bufferOffset)
    {
        uint32_t bufferIndex        = static_cast<uint32_t>(xfbIndex) + 1;
        mBufferSerials[bufferIndex] = bufferSerial;

        ASSERT(static_cast<uint64_t>(bufferOffset) <=
               static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()));
        mXfbBufferOffsets[xfbIndex] = static_cast<uint32_t>(bufferOffset);

        mBufferCount = std::max(mBufferCount, (bufferIndex + 1));
    }
    size_t hash() const;
    void reset();

    bool operator==(const UniformsAndXfbDescriptorDesc &other) const;

  private:
    uint32_t mBufferCount;
    // The array index 0 is used for default uniform buffer
    static constexpr size_t kDefaultUniformBufferIndex = 0;
    static constexpr size_t kDefaultUniformBufferCount = 1;
    static constexpr size_t kMaxBufferCount =
        kDefaultUniformBufferCount + gl::IMPLEMENTATION_MAX_TRANSFORM_FEEDBACK_BUFFERS;
    std::array<BufferSerial, kMaxBufferCount> mBufferSerials;
    std::array<uint32_t, gl::IMPLEMENTATION_MAX_TRANSFORM_FEEDBACK_BUFFERS> mXfbBufferOffsets;
};

class ShaderBuffersDescriptorDesc
{
  public:
    ShaderBuffersDescriptorDesc();
    ~ShaderBuffersDescriptorDesc();

    ShaderBuffersDescriptorDesc(const ShaderBuffersDescriptorDesc &other);
    ShaderBuffersDescriptorDesc &operator=(const ShaderBuffersDescriptorDesc &other);

    size_t hash() const;
    void reset();

    bool operator==(const ShaderBuffersDescriptorDesc &other) const;

    ANGLE_INLINE void appendBufferSerial(BufferSerial bufferSerial)
    {
        mPayload.push_back(bufferSerial.getValue());
    }
    ANGLE_INLINE void append32BitValue(uint32_t value) { mPayload.push_back(value); }

  private:
    // After a preliminary minimum size, use heap memory.
    static constexpr size_t kFastBufferWordLimit = 32;
    angle::FastVector<uint32_t, kFastBufferWordLimit> mPayload;
};

// In the FramebufferDesc object:
//  - Depth/stencil serial is at index 0
//  - Color serials are at indices [1, gl::IMPLEMENTATION_MAX_DRAW_BUFFERS]
//  - Depth/stencil resolve attachment is at index gl::IMPLEMENTATION_MAX_DRAW_BUFFERS+1
//  - Resolve attachments are at indices [gl::IMPLEMENTATION_MAX_DRAW_BUFFERS+2,
//                                        gl::IMPLEMENTATION_MAX_DRAW_BUFFERS*2+1]
constexpr size_t kFramebufferDescDepthStencilIndex = 0;
constexpr size_t kFramebufferDescColorIndexOffset  = kFramebufferDescDepthStencilIndex + 1;
constexpr size_t kFramebufferDescDepthStencilResolveIndexOffset =
    kFramebufferDescColorIndexOffset + gl::IMPLEMENTATION_MAX_DRAW_BUFFERS;
constexpr size_t kFramebufferDescColorResolveIndexOffset =
    kFramebufferDescDepthStencilResolveIndexOffset + 1;

// Enable struct padding warnings for the code below since it is used in caches.
ANGLE_ENABLE_STRUCT_PADDING_WARNINGS

class FramebufferDesc
{
  public:
    FramebufferDesc();
    ~FramebufferDesc();

    FramebufferDesc(const FramebufferDesc &other);
    FramebufferDesc &operator=(const FramebufferDesc &other);

    void updateColor(uint32_t index, ImageOrBufferViewSubresourceSerial serial);
    void updateColorResolve(uint32_t index, ImageOrBufferViewSubresourceSerial serial);
    void updateUnresolveMask(FramebufferNonResolveAttachmentMask unresolveMask);
    void updateDepthStencil(ImageOrBufferViewSubresourceSerial serial);
    void updateDepthStencilResolve(ImageOrBufferViewSubresourceSerial serial);
    ANGLE_INLINE void setWriteControlMode(gl::SrgbWriteControlMode mode)
    {
        mSrgbWriteControlMode = static_cast<uint16_t>(mode);
    }
    size_t hash() const;

    bool operator==(const FramebufferDesc &other) const;

    uint32_t attachmentCount() const;

    ImageOrBufferViewSubresourceSerial getColorImageViewSerial(uint32_t index)
    {
        ASSERT(kFramebufferDescColorIndexOffset + index < mSerials.size());
        return mSerials[kFramebufferDescColorIndexOffset + index];
    }

    FramebufferNonResolveAttachmentMask getUnresolveAttachmentMask() const;
    ANGLE_INLINE gl::SrgbWriteControlMode getWriteControlMode() const
    {
        return (mSrgbWriteControlMode == 1) ? gl::SrgbWriteControlMode::Linear
                                            : gl::SrgbWriteControlMode::Default;
    }

    void updateLayerCount(uint32_t layerCount);
    uint32_t getLayerCount() const { return mLayerCount; }
    void updateFramebufferFetchMode(bool hasFramebufferFetch);

    void updateRenderToTexture(bool isRenderToTexture);

  private:
    void reset();
    void update(uint32_t index, ImageOrBufferViewSubresourceSerial serial);

    // Note: this is an exclusive index. If there is one index it will be "1".
    // Maximum value is 18
    uint16_t mMaxIndex : 5;
    uint16_t mHasFramebufferFetch : 1;
    static_assert(gl::IMPLEMENTATION_MAX_FRAMEBUFFER_LAYERS < (1 << 9) - 1,
                  "Not enough bits for mLayerCount");

    uint16_t mLayerCount : 9;

    uint16_t mSrgbWriteControlMode : 1;

    // If the render pass contains an initial subpass to unresolve a number of attachments, the
    // subpass description is derived from the following mask, specifying which attachments need
    // to be unresolved.  Includes both color and depth/stencil attachments.
    uint16_t mUnresolveAttachmentMask : kMaxFramebufferNonResolveAttachments;

    // Whether this is a multisampled-render-to-single-sampled framebuffer.  Only used when using
    // VK_EXT_multisampled_render_to_single_sampled.  Only one bit is used and the rest is padding.
    uint16_t mIsRenderToTexture : 16 - kMaxFramebufferNonResolveAttachments;

    FramebufferAttachmentArray<ImageOrBufferViewSubresourceSerial> mSerials;
};

constexpr size_t kFramebufferDescSize = sizeof(FramebufferDesc);
static_assert(kFramebufferDescSize == 148, "Size check failed");

// Disable warnings about struct padding.
ANGLE_DISABLE_STRUCT_PADDING_WARNINGS

// The SamplerHelper allows a Sampler to be coupled with a serial.
// Must be included before we declare SamplerCache.
class SamplerHelper final : angle::NonCopyable
{
  public:
    SamplerHelper(ContextVk *contextVk);
    ~SamplerHelper();

    explicit SamplerHelper(SamplerHelper &&samplerHelper);
    SamplerHelper &operator=(SamplerHelper &&rhs);

    bool valid() const { return mSampler.valid(); }
    const Sampler &get() const { return mSampler; }
    Sampler &get() { return mSampler; }
    SamplerSerial getSamplerSerial() const { return mSamplerSerial; }

  private:
    Sampler mSampler;
    SamplerSerial mSamplerSerial;
};

using RefCountedSampler = RefCounted<SamplerHelper>;
using SamplerBinding    = BindingPointer<SamplerHelper>;

class RenderPassHelper final : angle::NonCopyable
{
  public:
    RenderPassHelper();
    ~RenderPassHelper();

    RenderPassHelper(RenderPassHelper &&other);
    RenderPassHelper &operator=(RenderPassHelper &&other);

    void destroy(VkDevice device);

    const RenderPass &getRenderPass() const;
    RenderPass &getRenderPass();

    const RenderPassPerfCounters &getPerfCounters() const;
    RenderPassPerfCounters &getPerfCounters();

  private:
    RenderPass mRenderPass;
    RenderPassPerfCounters mPerfCounters;
};
}  // namespace vk
}  // namespace rx

// Introduce std::hash for the above classes.
namespace std
{
template <>
struct hash<rx::vk::RenderPassDesc>
{
    size_t operator()(const rx::vk::RenderPassDesc &key) const { return key.hash(); }
};

template <>
struct hash<rx::vk::AttachmentOpsArray>
{
    size_t operator()(const rx::vk::AttachmentOpsArray &key) const { return key.hash(); }
};

template <>
struct hash<rx::vk::GraphicsPipelineDesc>
{
    size_t operator()(const rx::vk::GraphicsPipelineDesc &key) const { return key.hash(); }
};

template <>
struct hash<rx::vk::DescriptorSetLayoutDesc>
{
    size_t operator()(const rx::vk::DescriptorSetLayoutDesc &key) const { return key.hash(); }
};

template <>
struct hash<rx::vk::PipelineLayoutDesc>
{
    size_t operator()(const rx::vk::PipelineLayoutDesc &key) const { return key.hash(); }
};

template <>
struct hash<rx::vk::TextureDescriptorDesc>
{
    size_t operator()(const rx::vk::TextureDescriptorDesc &key) const { return key.hash(); }
};

template <>
struct hash<rx::vk::UniformsAndXfbDescriptorDesc>
{
    size_t operator()(const rx::vk::UniformsAndXfbDescriptorDesc &key) const { return key.hash(); }
};

template <>
struct hash<rx::vk::ShaderBuffersDescriptorDesc>
{
    size_t operator()(const rx::vk::ShaderBuffersDescriptorDesc &key) const { return key.hash(); }
};

template <>
struct hash<rx::vk::FramebufferDesc>
{
    size_t operator()(const rx::vk::FramebufferDesc &key) const { return key.hash(); }
};

template <>
struct hash<rx::vk::SamplerDesc>
{
    size_t operator()(const rx::vk::SamplerDesc &key) const { return key.hash(); }
};

// See Resource Serial types defined in vk_utils.h.
#define ANGLE_HASH_VK_SERIAL(Type)                                                          \
    template <>                                                                             \
    struct hash<rx::vk::Type##Serial>                                                       \
    {                                                                                       \
        size_t operator()(const rx::vk::Type##Serial &key) const { return key.getValue(); } \
    };

ANGLE_VK_SERIAL_OP(ANGLE_HASH_VK_SERIAL)

}  // namespace std

namespace rx
{
// Cache types for various Vulkan objects
enum class VulkanCacheType
{
    CompatibleRenderPass,
    RenderPassWithOps,
    GraphicsPipeline,
    PipelineLayout,
    Sampler,
    SamplerYcbcrConversion,
    DescriptorSetLayout,
    DriverUniformsDescriptors,
    TextureDescriptors,
    UniformsAndXfbDescriptors,
    ShaderBuffersDescriptors,
    Framebuffer,
    EnumCount
};

// Base class for all caches. Provides cache hit and miss counters.
class CacheStats final : angle::NonCopyable
{
  public:
    CacheStats() { reset(); }
    ~CacheStats() {}

    ANGLE_INLINE void hit() { mHitCount++; }
    ANGLE_INLINE void miss() { mMissCount++; }
    ANGLE_INLINE void accumulate(const CacheStats &stats)
    {
        mHitCount += stats.mHitCount;
        mMissCount += stats.mMissCount;
    }

    uint64_t getHitCount() const { return mHitCount; }
    uint64_t getMissCount() const { return mMissCount; }

    ANGLE_INLINE double getHitRatio() const
    {
        if (mHitCount + mMissCount == 0)
        {
            return 0;
        }
        else
        {
            return static_cast<double>(mHitCount) / (mHitCount + mMissCount);
        }
    }

    void reset()
    {
        mHitCount  = 0;
        mMissCount = 0;
    }

  private:
    uint64_t mHitCount;
    uint64_t mMissCount;
};

template <VulkanCacheType CacheType>
class HasCacheStats : angle::NonCopyable
{
  public:
    template <typename Accumulator>
    void accumulateCacheStats(Accumulator *accum)
    {
        accum->accumulateCacheStats(CacheType, mCacheStats);
        mCacheStats.reset();
    }

  protected:
    HasCacheStats()          = default;
    virtual ~HasCacheStats() = default;

    CacheStats mCacheStats;
};

// TODO(jmadill): Add cache trimming/eviction.
class RenderPassCache final : angle::NonCopyable
{
  public:
    RenderPassCache();
    ~RenderPassCache();

    void destroy(RendererVk *rendererVk);

    ANGLE_INLINE angle::Result getCompatibleRenderPass(ContextVk *contextVk,
                                                       const vk::RenderPassDesc &desc,
                                                       vk::RenderPass **renderPassOut)
    {
        auto outerIt = mPayload.find(desc);
        if (outerIt != mPayload.end())
        {
            InnerCache &innerCache = outerIt->second;
            ASSERT(!innerCache.empty());

            // Find the first element and return it.
            *renderPassOut = &innerCache.begin()->second.getRenderPass();
            mCompatibleRenderPassCacheStats.hit();
            return angle::Result::Continue;
        }

        mCompatibleRenderPassCacheStats.miss();
        return addRenderPass(contextVk, desc, renderPassOut);
    }

    angle::Result getRenderPassWithOps(ContextVk *contextVk,
                                       const vk::RenderPassDesc &desc,
                                       const vk::AttachmentOpsArray &attachmentOps,
                                       vk::RenderPass **renderPassOut);

  private:
    angle::Result getRenderPassWithOpsImpl(ContextVk *contextVk,
                                           const vk::RenderPassDesc &desc,
                                           const vk::AttachmentOpsArray &attachmentOps,
                                           bool updatePerfCounters,
                                           vk::RenderPass **renderPassOut);

    angle::Result addRenderPass(ContextVk *contextVk,
                                const vk::RenderPassDesc &desc,
                                vk::RenderPass **renderPassOut);

    // Use a two-layer caching scheme. The top level matches the "compatible" RenderPass elements.
    // The second layer caches the attachment load/store ops and initial/final layout.
    using InnerCache = angle::HashMap<vk::AttachmentOpsArray, vk::RenderPassHelper>;
    using OuterCache = angle::HashMap<vk::RenderPassDesc, InnerCache>;

    OuterCache mPayload;
    CacheStats mCompatibleRenderPassCacheStats;
    CacheStats mRenderPassWithOpsCacheStats;
};

// TODO(jmadill): Add cache trimming/eviction.
class GraphicsPipelineCache final : public HasCacheStats<VulkanCacheType::GraphicsPipeline>
{
  public:
    GraphicsPipelineCache();
    ~GraphicsPipelineCache() override;

    void destroy(RendererVk *rendererVk);
    void release(ContextVk *context);

    void populate(const vk::GraphicsPipelineDesc &desc, vk::Pipeline &&pipeline);

    ANGLE_INLINE angle::Result getPipeline(ContextVk *contextVk,
                                           const vk::PipelineCache &pipelineCacheVk,
                                           const vk::RenderPass &compatibleRenderPass,
                                           const vk::PipelineLayout &pipelineLayout,
                                           const gl::AttributesMask &activeAttribLocationsMask,
                                           const gl::ComponentTypeMask &programAttribsTypeMask,
                                           const vk::ShaderModule *vertexModule,
                                           const vk::ShaderModule *fragmentModule,
                                           const vk::ShaderModule *geometryModule,
                                           const vk::ShaderModule *tessControlModule,
                                           const vk::ShaderModule *tessEvaluationModule,
                                           const vk::SpecializationConstants &specConsts,
                                           const vk::GraphicsPipelineDesc &desc,
                                           const vk::GraphicsPipelineDesc **descPtrOut,
                                           vk::PipelineHelper **pipelineOut)
    {
        auto item = mPayload.find(desc);
        if (item != mPayload.end())
        {
            *descPtrOut  = &item->first;
            *pipelineOut = &item->second;
            mCacheStats.hit();
            return angle::Result::Continue;
        }

        mCacheStats.miss();
        return insertPipeline(contextVk, pipelineCacheVk, compatibleRenderPass, pipelineLayout,
                              activeAttribLocationsMask, programAttribsTypeMask, vertexModule,
                              fragmentModule, geometryModule, tessControlModule,
                              tessEvaluationModule, specConsts, desc, descPtrOut, pipelineOut);
    }

  private:
    angle::Result insertPipeline(ContextVk *contextVk,
                                 const vk::PipelineCache &pipelineCacheVk,
                                 const vk::RenderPass &compatibleRenderPass,
                                 const vk::PipelineLayout &pipelineLayout,
                                 const gl::AttributesMask &activeAttribLocationsMask,
                                 const gl::ComponentTypeMask &programAttribsTypeMask,
                                 const vk::ShaderModule *vertexModule,
                                 const vk::ShaderModule *fragmentModule,
                                 const vk::ShaderModule *geometryModule,
                                 const vk::ShaderModule *tessControlModule,
                                 const vk::ShaderModule *tessEvaluationModule,
                                 const vk::SpecializationConstants &specConsts,
                                 const vk::GraphicsPipelineDesc &desc,
                                 const vk::GraphicsPipelineDesc **descPtrOut,
                                 vk::PipelineHelper **pipelineOut);

    std::unordered_map<vk::GraphicsPipelineDesc, vk::PipelineHelper> mPayload;
};

class DescriptorSetLayoutCache final : angle::NonCopyable
{
  public:
    DescriptorSetLayoutCache();
    ~DescriptorSetLayoutCache();

    void destroy(RendererVk *rendererVk);

    angle::Result getDescriptorSetLayout(
        vk::Context *context,
        const vk::DescriptorSetLayoutDesc &desc,
        vk::BindingPointer<vk::DescriptorSetLayout> *descriptorSetLayoutOut);

  private:
    std::unordered_map<vk::DescriptorSetLayoutDesc, vk::RefCountedDescriptorSetLayout> mPayload;
    CacheStats mCacheStats;
};

class PipelineLayoutCache final : public HasCacheStats<VulkanCacheType::PipelineLayout>
{
  public:
    PipelineLayoutCache();
    ~PipelineLayoutCache() override;

    void destroy(RendererVk *rendererVk);

    angle::Result getPipelineLayout(vk::Context *context,
                                    const vk::PipelineLayoutDesc &desc,
                                    const vk::DescriptorSetLayoutPointerArray &descriptorSetLayouts,
                                    vk::BindingPointer<vk::PipelineLayout> *pipelineLayoutOut);

  private:
    std::unordered_map<vk::PipelineLayoutDesc, vk::RefCountedPipelineLayout> mPayload;
};

class SamplerCache final : public HasCacheStats<VulkanCacheType::Sampler>
{
  public:
    SamplerCache();
    ~SamplerCache() override;

    void destroy(RendererVk *rendererVk);

    angle::Result getSampler(ContextVk *contextVk,
                             const vk::SamplerDesc &desc,
                             vk::SamplerBinding *samplerOut);

  private:
    std::unordered_map<vk::SamplerDesc, vk::RefCountedSampler> mPayload;
};

// YuvConversion Cache
class SamplerYcbcrConversionCache final
    : public HasCacheStats<VulkanCacheType::SamplerYcbcrConversion>
{
  public:
    SamplerYcbcrConversionCache();
    ~SamplerYcbcrConversionCache() override;

    void destroy(RendererVk *rendererVk);

    angle::Result getYuvConversion(
        vk::Context *context,
        uint64_t externalFormat,
        const VkSamplerYcbcrConversionCreateInfo &yuvConversionCreateInfo,
        vk::BindingPointer<vk::SamplerYcbcrConversion> *yuvConversionOut);
    VkSamplerYcbcrConversion getYuvConversionFromExternalFormat(uint64_t externalFormat) const;

  private:
    std::unordered_map<uint64_t, vk::RefCountedSamplerYcbcrConversion> mPayload;
};

// DescriptorSet Cache
class DriverUniformsDescriptorSetCache final
    : public HasCacheStats<VulkanCacheType::DriverUniformsDescriptors>
{
  public:
    DriverUniformsDescriptorSetCache() = default;
    ~DriverUniformsDescriptorSetCache() override { ASSERT(mPayload.empty()); }

    void destroy(RendererVk *rendererVk);

    ANGLE_INLINE bool get(uint32_t serial, VkDescriptorSet *descriptorSet)
    {
        if (mPayload.get(serial, descriptorSet))
        {
            mCacheStats.hit();
            return true;
        }
        mCacheStats.miss();
        return false;
    }

    ANGLE_INLINE void insert(uint32_t serial, VkDescriptorSet descriptorSet)
    {
        mPayload.insert(serial, descriptorSet);
    }

    ANGLE_INLINE void clear() { mPayload.clear(); }

  private:
    angle::FastIntegerMap<VkDescriptorSet> mPayload;
};

// Templated Descriptors Cache
template <typename Key, VulkanCacheType CacheType>
class DescriptorSetCache final : public HasCacheStats<CacheType>
{
  public:
    DescriptorSetCache() = default;
    ~DescriptorSetCache() override { ASSERT(mPayload.empty()); }

    void destroy(RendererVk *rendererVk);

    ANGLE_INLINE bool get(const Key &desc, VkDescriptorSet *descriptorSet)
    {
        auto iter = mPayload.find(desc);
        if (iter != mPayload.end())
        {
            *descriptorSet = iter->second;
            this->mCacheStats.hit();
            return true;
        }
        this->mCacheStats.miss();
        return false;
    }

    ANGLE_INLINE void insert(const Key &desc, VkDescriptorSet descriptorSet)
    {
        mPayload.emplace(desc, descriptorSet);
    }

  private:
    angle::HashMap<Key, VkDescriptorSet> mPayload;
};

// Only 1 driver uniform binding is used.
constexpr uint32_t kReservedDriverUniformBindingCount = 1;
// There is 1 default uniform binding used per stage.  Currently, a maxium of three stages are
// supported.
constexpr uint32_t kReservedPerStageDefaultUniformBindingCount = 1;
constexpr uint32_t kReservedDefaultUniformBindingCount         = 3;
}  // namespace rx

#endif  // LIBANGLE_RENDERER_VULKAN_VK_CACHE_UTILS_H_
