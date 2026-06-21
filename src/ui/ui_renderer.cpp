#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#endif

#include <fstream>
#include <filesystem>

#include <concurrentqueue.h>

#include "rt64_render_hooks.h"
#include "rt64_render_interface_builders.h"

#include "RmlUi/Core/RenderInterfaceCompatibility.h"

#include "ui_renderer.h"

#include "../../lib/rt64/src/contrib/stb/stb_image.h"

#include "InterfaceVS.hlsl.spirv.h"
#include "InterfacePS.hlsl.spirv.h"

#ifdef _WIN32
#   include "InterfaceVS.hlsl.dxil.h"
#   include "InterfacePS.hlsl.dxil.h"
#elif defined(__APPLE__)
#   include "InterfaceVS.hlsl.metal.h"
#   include "InterfacePS.hlsl.metal.h"
#endif

#ifdef _WIN32
#    define GET_SHADER_BLOB(name, format) \
        ((format) == RT64::RenderShaderFormat::SPIRV ? name##BlobSPIRV : \
        (format) == RT64::RenderShaderFormat::DXIL ? name##BlobDXIL : nullptr)
#    define GET_SHADER_SIZE(name, format) \
        ((format) == RT64::RenderShaderFormat::SPIRV ? std::size(name##BlobSPIRV) : \
        (format) == RT64::RenderShaderFormat::DXIL ? std::size(name##BlobDXIL) : 0)
#elif defined(__APPLE__)
#    define GET_SHADER_BLOB(name, format) \
((format) == RT64::RenderShaderFormat::SPIRV ? name##BlobSPIRV : \
(format) == RT64::RenderShaderFormat::METAL ? name##BlobMSL : nullptr)
#    define GET_SHADER_SIZE(name, format) \
((format) == RT64::RenderShaderFormat::SPIRV ? std::size(name##BlobSPIRV) : \
(format) == RT64::RenderShaderFormat::METAL ? std::size(name##BlobMSL) : 0)
#else
#    define GET_SHADER_BLOB(name, format) \
        ((format) == RT64::RenderShaderFormat::SPIRV ? name##BlobSPIRV : nullptr)
#    define GET_SHADER_SIZE(name, format) \
        ((format) == RT64::RenderShaderFormat::SPIRV ? std::size(name##BlobSPIRV) : 0)
#endif

// TODO deduplicate from rt64_common.h
void CalculateTextureRowWidthPadding(uint32_t rowPitch, uint32_t &rowWidth, uint32_t &rowPadding) {
    const int RowMultiple = 256;
    rowWidth = rowPitch;
    rowPadding = (rowWidth % RowMultiple) ? RowMultiple - (rowWidth % RowMultiple) : 0;
    rowWidth += rowPadding;
}

struct RmlPushConstants {
    Rml::Matrix4f transform;
    Rml::Vector2f translation;
};

struct TextureHandle {
    std::unique_ptr<RT64::RenderTexture> texture;
    std::unique_ptr<RT64::RenderDescriptorSet> set;
    bool transitioned = false;
};

template <typename T>
T from_bytes_le(const char* input) {
    return *reinterpret_cast<const T*>(input);
}

enum class ImageType {
    File,
    RGBA32
};

struct ImageFromBytes {
    ImageType type;
    // Dimensions only used for RGBA32 data. Files pull the size from the file data. 
    uint32_t width;
    uint32_t height;
    std::string name;
    std::vector<char> bytes;
};

namespace recompui {
class RmlRenderInterface_RT64_impl : public Rml::RenderInterfaceCompatibility {
    struct DynamicBuffer {
        std::unique_ptr<RT64::RenderBuffer> buffer_{};
        uint32_t size_ = 0;
        uint32_t bytes_used_ = 0;
        uint8_t* mapped_data_ = nullptr;
        RT64::RenderBufferFlags flags_ = RT64::RenderBufferFlag::NONE;
    };

    static constexpr uint32_t per_frame_descriptor_set = 0;
    static constexpr uint32_t per_draw_descriptor_set = 1;

    static constexpr uint32_t initial_upload_buffer_size = 1024 * 1024;
    static constexpr uint32_t initial_vertex_buffer_size = 512 * sizeof(Rml::Vertex);
    static constexpr uint32_t initial_index_buffer_size = 1024 * sizeof(int);
    static constexpr RT64::RenderFormat RmlTextureFormat = RT64::RenderFormat::R8G8B8A8_UNORM;
    static constexpr RT64::RenderFormat RmlTextureFormatBgra = RT64::RenderFormat::B8G8R8A8_UNORM;
    static constexpr RT64::RenderFormat SwapChainFormat = RT64::RenderFormat::B8G8R8A8_UNORM;
    static constexpr uint32_t RmlTextureFormatBytesPerPixel = RenderFormatSize(RmlTextureFormat);
    static_assert(RenderFormatSize(RmlTextureFormatBgra) == RmlTextureFormatBytesPerPixel);
    RT64::RenderInterface* interface_;
    RT64::RenderDevice* device_;
    int scissor_x_ = 0;
    int scissor_y_ = 0;
    int scissor_width_ = 0;
    int scissor_height_ = 0;
    int window_width_ = 0;
    int window_height_ = 0;
    RT64::RenderMultisampling multisampling_ = RT64::RenderMultisampling();
    Rml::Matrix4f projection_mtx_ = Rml::Matrix4f::Identity();
    Rml::Matrix4f transform_ = Rml::Matrix4f::Identity();
    Rml::Matrix4f mvp_ = Rml::Matrix4f::Identity();
    std::unordered_map<Rml::TextureHandle, TextureHandle> textures_{};
    Rml::TextureHandle texture_count_ = 2; // Start at 1 to reserve texture 0 as the 1x1 pixel white texture
    DynamicBuffer upload_buffer_;
    DynamicBuffer vertex_buffer_;
    DynamicBuffer index_buffer_;
    std::unique_ptr<RT64::RenderSampler> nearestSampler_{};
    std::unique_ptr<RT64::RenderSampler> linearSampler_{};
    std::unique_ptr<RT64::RenderShader> vertex_shader_{};
    std::unique_ptr<RT64::RenderShader> pixel_shader_{};
    std::unique_ptr<RT64::RenderDescriptorSet> sampler_set_{};
    std::unique_ptr<RT64::RenderDescriptorSetBuilder> texture_set_builder_{};
    std::unique_ptr<RT64::RenderPipelineLayout> layout_{};
    std::unique_ptr<RT64::RenderPipeline> pipeline_{};
    std::unique_ptr<RT64::RenderPipeline> pipeline_ms_{};
    std::unique_ptr<RT64::RenderTexture> screen_texture_ms_{};
    std::unique_ptr<RT64::RenderTexture> screen_texture_{};
    std::unique_ptr<RT64::RenderFramebuffer> screen_framebuffer_{};
    std::unique_ptr<RT64::RenderDescriptorSet> screen_descriptor_set_{};
    std::unique_ptr<RT64::RenderBuffer> screen_vertex_buffer_{};
    uint64_t screen_vertex_buffer_size_ = 0;
    uint32_t gTexture_descriptor_index;
    RT64::RenderInputSlot vertex_slot_{ 0, sizeof(Rml::Vertex) };
    RT64::RenderCommandList* list_ = nullptr;
    bool scissor_enabled_ = false;
    std::vector<std::unique_ptr<RT64::RenderBuffer>> stale_buffers_{};
    moodycamel::ConcurrentQueue<ImageFromBytes> image_from_bytes_queue;
    std::unordered_map<std::string, ImageFromBytes> image_from_bytes_map;
    std::unordered_map<std::string, Rml::TextureHandle> loaded_source_handles_;
    std::vector<std::string> pending_texture_sources_;

    static Rml::Vector2i query_image_dimensions(const ImageFromBytes& img) {
        switch (img.type) {
            case ImageType::RGBA32:
                return { int(img.width), int(img.height) };
            case ImageType::File: {
                int width = 0;
                int height = 0;
                int channels = 0;
                if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(img.bytes.data()),
                        int(img.bytes.size()), &width, &height, &channels)) {
                    return { 1, 1 };
                }
                return { width, height };
            }
        }
        return { 1, 1 };
    }

    bool upload_image_to_handle(const ImageFromBytes& img, Rml::TextureHandle texture_handle) {
        if (textures_.contains(texture_handle)) {
            return true;
        }

        switch (img.type) {
            case ImageType::RGBA32:
                return create_texture(texture_handle, reinterpret_cast<const Rml::byte*>(img.bytes.data()),
                    { int(img.width), int(img.height) });
            case ImageType::File: {
                int width = 0;
                int height = 0;
                stbi_uc* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(img.bytes.data()),
                    int(img.bytes.size()), &width, &height, nullptr, 4);
                if (pixels == nullptr) {
                    return false;
                }
                const bool uploaded = create_texture(texture_handle, pixels, { width, height });
                stbi_image_free(pixels);
                return uploaded;
            }
        }
        return false;
    }

    void upload_pending_textures() {
        if (list_ == nullptr || pending_texture_sources_.empty()) {
            return;
        }

        for (const std::string& source : pending_texture_sources_) {
            const auto image_it = image_from_bytes_map.find(source);
            const auto handle_it = loaded_source_handles_.find(source);
            if (image_it == image_from_bytes_map.end() || handle_it == loaded_source_handles_.end()) {
                continue;
            }
            upload_image_to_handle(image_it->second, handle_it->second);
        }

        pending_texture_sources_.clear();
    }

    void ensure_builtin_textures() {
        if (!textures_.contains(0)) {
            Rml::byte white_pixel[] = { 255, 255, 255, 255 };
            create_texture(0, white_pixel, { 1, 1 });
        }
        if (!textures_.contains(1)) {
            Rml::byte transparent_pixel[] = { 0, 0, 0, 0 };
            create_texture(1, transparent_pixel, { 1, 1 });
        }
    }

public:
    RmlRenderInterface_RT64_impl(RT64::RenderInterface* interface, RT64::RenderDevice* device) {
        interface_ = interface;
        device_ = device;

        // MSAA offscreen resolve has caused D3D12 present failures on some drivers for this port.
        multisampling_ = RT64::RenderMultisampling();

        vertex_buffer_.flags_ = RT64::RenderBufferFlag::VERTEX;
        index_buffer_.flags_ = RT64::RenderBufferFlag::INDEX;

        // Create the texture upload buffer, vertex buffer and index buffer
        resize_dynamic_buffer(upload_buffer_, initial_upload_buffer_size, false);
        resize_dynamic_buffer(vertex_buffer_, initial_vertex_buffer_size, false);
        resize_dynamic_buffer(index_buffer_, initial_index_buffer_size, false);

        // Describe the vertex format
        std::vector<RT64::RenderInputElement> vertex_elements{};
        vertex_elements.emplace_back(RT64::RenderInputElement{ "POSITION", 0, 0, RT64::RenderFormat::R32G32_FLOAT, 0, offsetof(Rml::Vertex, position) });
        vertex_elements.emplace_back(RT64::RenderInputElement{ "COLOR", 0, 1, RT64::RenderFormat::R8G8B8A8_UNORM, 0, offsetof(Rml::Vertex, colour) });
        vertex_elements.emplace_back(RT64::RenderInputElement{ "TEXCOORD", 0, 2, RT64::RenderFormat::R32G32_FLOAT, 0, offsetof(Rml::Vertex, tex_coord) });

        // Create a nearest sampler and a linear sampler
        RT64::RenderSamplerDesc samplerDesc;
        samplerDesc.minFilter = RT64::RenderFilter::NEAREST;
        samplerDesc.magFilter = RT64::RenderFilter::NEAREST;
        samplerDesc.addressU = RT64::RenderTextureAddressMode::CLAMP;
        samplerDesc.addressV = RT64::RenderTextureAddressMode::CLAMP;
        samplerDesc.addressW = RT64::RenderTextureAddressMode::CLAMP;
        nearestSampler_ = device_->createSampler(samplerDesc);

        samplerDesc.minFilter = RT64::RenderFilter::LINEAR;
        samplerDesc.magFilter = RT64::RenderFilter::LINEAR;
        linearSampler_ = device_->createSampler(samplerDesc);

        // Create the shaders
        RT64::RenderShaderFormat shaderFormat = interface_->getCapabilities().shaderFormat;

        vertex_shader_ = device_->createShader(GET_SHADER_BLOB(InterfaceVS, shaderFormat), GET_SHADER_SIZE(InterfaceVS, shaderFormat), "VSMain", shaderFormat);
        pixel_shader_ = device_->createShader(GET_SHADER_BLOB(InterfacePS, shaderFormat), GET_SHADER_SIZE(InterfacePS, shaderFormat), "PSMain", shaderFormat);


        // Create the descriptor set that contains the sampler
        RT64::RenderDescriptorSetBuilder sampler_set_builder{};
        sampler_set_builder.begin();
        sampler_set_builder.addImmutableSampler(1, linearSampler_.get());
        sampler_set_builder.addConstantBuffer(3, 1); // Workaround D3D12 crash due to an empty RT64 descriptor set
        sampler_set_builder.end();
        sampler_set_ = sampler_set_builder.create(device_);

        // Create a builder for the descriptor sets that will contain textures
        texture_set_builder_ = std::make_unique<RT64::RenderDescriptorSetBuilder>();
        texture_set_builder_->begin();
        gTexture_descriptor_index = texture_set_builder_->addTexture(2);
        texture_set_builder_->end();

        // Create the pipeline layout
        RT64::RenderPipelineLayoutBuilder layout_builder{};
        layout_builder.begin(false, true);
        layout_builder.addPushConstant(0, 0, sizeof(RmlPushConstants), RT64::RenderShaderStageFlag::VERTEX);
        // Add the descriptor set for descriptors changed once per frame.
        layout_builder.addDescriptorSet(sampler_set_builder);
        // Add the descriptor set for descriptors changed once per draw.
        layout_builder.addDescriptorSet(*texture_set_builder_);
        layout_builder.end();
        layout_ = layout_builder.create(device_);

        // Create the pipeline description
        RT64::RenderGraphicsPipelineDesc pipeline_desc{};
        // Set up alpha blending for non-premultiplied alpha. RmlUi recommends using premultiplied alpha normally,
        // but that would require preprocessing all input files, which would be difficult for user-provided content (such as mods).
        // This blending setup produces similar results as premultipled alpha but for normal assets as it multiplies during blending and
        // computes the output alpha value the same way that a premultipled alpha blender would.
        pipeline_desc.renderTargetBlend[0] = RT64::RenderBlendDesc {
            .blendEnabled = true,
            .srcBlend = RT64::RenderBlend::SRC_ALPHA,
            .dstBlend = RT64::RenderBlend::INV_SRC_ALPHA,
            .blendOp = RT64::RenderBlendOperation::ADD,
            .srcBlendAlpha = RT64::RenderBlend::ONE,
            .dstBlendAlpha = RT64::RenderBlend::INV_SRC_ALPHA,
            .blendOpAlpha = RT64::RenderBlendOperation::ADD,
        };
        pipeline_desc.renderTargetFormat[0] = SwapChainFormat; // TODO: Use whatever format the swap chain was created with.
        pipeline_desc.renderTargetCount = 1;
        pipeline_desc.cullMode = RT64::RenderCullMode::NONE;
        pipeline_desc.inputSlots = &vertex_slot_;
        pipeline_desc.inputSlotsCount = 1;
        pipeline_desc.inputElements = vertex_elements.data();
        pipeline_desc.inputElementsCount = uint32_t(vertex_elements.size());
        pipeline_desc.pipelineLayout = layout_.get();
        pipeline_desc.primitiveTopology = RT64::RenderPrimitiveTopology::TRIANGLE_LIST;
        pipeline_desc.vertexShader = vertex_shader_.get();
        pipeline_desc.pixelShader = pixel_shader_.get();

        pipeline_ = device_->createGraphicsPipeline(pipeline_desc);

        if (multisampling_.sampleCount > 1) {
            pipeline_desc.multisampling = multisampling_;
            pipeline_ms_ = device_->createGraphicsPipeline(pipeline_desc);

            // Create the descriptor set for the screen drawer.
            RT64::RenderDescriptorRange screen_descriptor_range(RT64::RenderDescriptorRangeType::TEXTURE, 2, 1);
            screen_descriptor_set_ = device_->createDescriptorSet(RT64::RenderDescriptorSetDesc(&screen_descriptor_range, 1));

            // Create vertex buffer for the screen drawer (full-screen triangle).
            screen_vertex_buffer_size_ = sizeof(Rml::Vertex) * 3;
            screen_vertex_buffer_ = device_->createBuffer(RT64::RenderBufferDesc::VertexBuffer(screen_vertex_buffer_size_, RT64::RenderHeapType::UPLOAD));
            Rml::Vertex *vertices = (Rml::Vertex *)(screen_vertex_buffer_->map());
            const Rml::ColourbPremultiplied white(255, 255, 255, 255);
            vertices[0] = Rml::Vertex{ Rml::Vector2f(-1.0f, 1.0f), white, Rml::Vector2f(0.0f, 0.0f) };
            vertices[1] = Rml::Vertex{ Rml::Vector2f(-1.0f, -3.0f), white, Rml::Vector2f(0.0f, 2.0f) };
            vertices[2] = Rml::Vertex{ Rml::Vector2f(3.0f, 1.0f), white, Rml::Vector2f(2.0f, 0.0f) };
            screen_vertex_buffer_->unmap();
        }

        // Builtin textures are uploaded on the first UI draw when the graphics command list is active.
    }

    void sync_upload_pending_textures() {
        assert(list_ == nullptr && "sync_upload_pending_textures must run outside an active UI command list");
        upload_pending_textures();
    }

    void reset_dynamic_buffer(DynamicBuffer &dynamic_buffer) {
        assert(dynamic_buffer.mapped_data_ == nullptr);
        dynamic_buffer.bytes_used_ = 0;
        dynamic_buffer.mapped_data_ = reinterpret_cast<uint8_t*>(dynamic_buffer.buffer_->map());
    }

    void end_dynamic_buffer(DynamicBuffer &dynamic_buffer) {
        assert(dynamic_buffer.mapped_data_ != nullptr);
        dynamic_buffer.buffer_->unmap();
        dynamic_buffer.mapped_data_ = nullptr;
    }

    void resize_dynamic_buffer(DynamicBuffer &dynamic_buffer, uint32_t new_size, bool map = true) {
        // Unmap the buffer if it's mapped
        if (dynamic_buffer.mapped_data_ != nullptr) {
            dynamic_buffer.buffer_->unmap();
        }
        
        // If there's already a buffer, move it into the stale buffers so it persists until the start of next frame.
        if (dynamic_buffer.buffer_ != nullptr) {
            stale_buffers_.emplace_back(std::move(dynamic_buffer.buffer_));
        }

        // Create the new buffer, update the size and map it.
        dynamic_buffer.buffer_ = device_->createBuffer(RT64::RenderBufferDesc::UploadBuffer(new_size, dynamic_buffer.flags_));
        dynamic_buffer.size_ = new_size;
        dynamic_buffer.bytes_used_ = 0;

        if (map) {
            dynamic_buffer.mapped_data_ = reinterpret_cast<uint8_t*>(dynamic_buffer.buffer_->map());
        }
    }

    uint32_t allocate_dynamic_data(DynamicBuffer &dynamic_buffer, uint32_t num_bytes) {
        // Check if there's enough remaining room in the buffer to allocate the requested bytes.
        uint32_t total_bytes = num_bytes + dynamic_buffer.bytes_used_;

        if (total_bytes > dynamic_buffer.size_) {
            // There isn't, so mark the current buffer as stale and allocate a new one with 50% more space than the required amount.
            resize_dynamic_buffer(dynamic_buffer, total_bytes + total_bytes / 2);
        }

        // Record the current end of the buffer to return.
        uint32_t offset = dynamic_buffer.bytes_used_;

        // Bump the buffer's end forward by the number of bytes allocated.
        dynamic_buffer.bytes_used_ += num_bytes;

        return offset;
    }

    uint32_t allocate_dynamic_data_aligned(DynamicBuffer &dynamic_buffer, uint32_t num_bytes, uint32_t alignment) {
        // Check if there's enough remaining room in the buffer to allocate the requested bytes.
        uint32_t total_bytes = num_bytes + dynamic_buffer.bytes_used_;

        // Determine the amount of padding needed to meet the target alignment.
        uint32_t padding_bytes = ((dynamic_buffer.bytes_used_ + alignment - 1) / alignment) * alignment - dynamic_buffer.bytes_used_;

        // If there isn't enough room to allocate the required bytes plus the padding then resize the buffer and allocate from the start of the new one.
        if (total_bytes + padding_bytes > dynamic_buffer.size_) {
            resize_dynamic_buffer(dynamic_buffer, total_bytes + total_bytes / 2);

            dynamic_buffer.bytes_used_ += num_bytes;

            return 0;
        }

        // Otherwise allocate the padding and required bytes and offset the allocated position by the padding size.
        return allocate_dynamic_data(dynamic_buffer, padding_bytes + num_bytes) + padding_bytes;
    }
    
    void RenderGeometry(Rml::Vertex* vertices, int num_vertices, int* indices, int num_indices, Rml::TextureHandle texture, const Rml::Vector2f& translation) override {
        if (!textures_.contains(texture)) {
            if (texture == 0) {
                Rml::byte white_pixel[] = { 255, 255, 255, 255 };
                create_texture(0, white_pixel, Rml::Vector2i{ 1, 1 });
            }
            else if (texture == 1) {
                Rml::byte transparent_pixel[] = { 0, 0, 0, 0 };
                create_texture(1, transparent_pixel, Rml::Vector2i{ 1, 1 });
            }
            else {
                return;
            }
        }

        if (!textures_.contains(texture)) {
            return;
        }

        // Copy the vertex and index data into the mapped buffers.
        uint32_t vert_size_bytes = num_vertices * sizeof(*vertices);
        uint32_t index_size_bytes = num_indices * sizeof(*indices);
        uint32_t vertex_buffer_offset = allocate_dynamic_data(vertex_buffer_, vert_size_bytes);
        uint32_t index_buffer_offset = allocate_dynamic_data(index_buffer_, index_size_bytes);
        memcpy(vertex_buffer_.mapped_data_ + vertex_buffer_offset, vertices, vert_size_bytes);
        memcpy(index_buffer_.mapped_data_ + index_buffer_offset, indices, index_size_bytes);

        list_->setViewports(RT64::RenderViewport{ 0, 0, float(window_width_), float(window_height_) });
        if (scissor_enabled_) {
            list_->setScissors(RT64::RenderRect{
                scissor_x_,
                scissor_y_,
                (scissor_width_ + scissor_x_),
                (scissor_height_ + scissor_y_) });
        }
        else {
            list_->setScissors(RT64::RenderRect{ 0, 0, window_width_, window_height_ });
        }

        RT64::RenderIndexBufferView index_view{index_buffer_.buffer_->at(index_buffer_offset), index_size_bytes, RT64::RenderFormat::R32_UINT};
        list_->setIndexBuffer(&index_view);
        RT64::RenderVertexBufferView vertex_view{vertex_buffer_.buffer_->at(vertex_buffer_offset), vert_size_bytes};
        list_->setVertexBuffers(0, &vertex_view, 1, &vertex_slot_);

        TextureHandle &texture_handle = textures_.at(texture);
        if (!texture_handle.transitioned) {
            // Prepare the texture for being read from a pixel shader.
            list_->barriers(RT64::RenderBarrierStage::GRAPHICS, RT64::RenderTextureBarrier(texture_handle.texture.get(), RT64::RenderTextureLayout::SHADER_READ));
            texture_handle.transitioned = true;
        }

        list_->setGraphicsDescriptorSet(texture_handle.set.get(), 1);

        RmlPushConstants constants{
            .transform = mvp_,
            .translation = translation
        };

        list_->setGraphicsPushConstants(0, &constants);

        list_->drawIndexedInstanced(num_indices, 1, 0, 0, 0);
    }

    void EnableScissorRegion(bool enable) override {
        scissor_enabled_ = enable;
    }

    void SetScissorRegion(int x, int y, int width, int height) override {
        scissor_x_ = x;
        scissor_y_ = y;
        scissor_width_ = width;
        scissor_height_ = height;
    }

    bool LoadTexture(Rml::TextureHandle& texture_handle, Rml::Vector2i& texture_dimensions, const Rml::String& source) override {
        flush_image_from_bytes_queue();

        const auto it = image_from_bytes_map.find(source);
        if (it == image_from_bytes_map.end()) {
            texture_handle = 1;
            texture_dimensions.x = 1;
            texture_dimensions.y = 1;
            return true;
        }

        const auto existing_handle = loaded_source_handles_.find(source);
        if (existing_handle != loaded_source_handles_.end()) {
            texture_handle = existing_handle->second;
            texture_dimensions = query_image_dimensions(it->second);
            return true;
        }

        // Defer GPU upload until start() when the UI command list is active. Uploading from
        // LoadDocument (during init_hook) via a separate copy queue corrupts D3D12 state.
        texture_handle = texture_count_++;
        texture_dimensions = query_image_dimensions(it->second);
        loaded_source_handles_.emplace(source, texture_handle);
        pending_texture_sources_.push_back(source);
        return true;
    }

    bool GenerateTexture(Rml::TextureHandle& texture_handle, const Rml::byte* source, const Rml::Vector2i& source_dimensions) override {
        if (source_dimensions.x == 0 || source_dimensions.y == 0) {
            texture_handle = 0;
            return true;
        }

        texture_handle = texture_count_++;
        return create_texture(texture_handle, source, source_dimensions);
    }

    bool create_texture(Rml::TextureHandle texture_handle, const Rml::byte* source, const Rml::Vector2i& source_dimensions, bool flip_y = false, bool bgra = false) {
        (void)bgra;

        const uint32_t row_pitch = source_dimensions.x * RmlTextureFormatBytesPerPixel;
        const size_t byte_count = source_dimensions.y * row_pitch;

        std::vector<uint8_t> source_copy;
        const uint8_t* src_bytes = reinterpret_cast<const uint8_t*>(source);
        if (flip_y) {
            source_copy.resize(byte_count);
            for (int row = 0; row < source_dimensions.y; row++) {
                memcpy(source_copy.data() + row * row_pitch, source + (source_dimensions.y - row - 1) * row_pitch, row_pitch);
            }
            src_bytes = source_copy.data();
        }

        if (list_ == nullptr) {
            return false;
        }

        constexpr uint32_t row_pitch_alignment = 256;
        const uint32_t aligned_row_pitch = ((row_pitch + row_pitch_alignment - 1) / row_pitch_alignment) * row_pitch_alignment;
        const uint32_t aligned_row_width = aligned_row_pitch / RmlTextureFormatBytesPerPixel;

        std::unique_ptr<RT64::RenderTexture> texture = device_->createTexture(
            RT64::RenderTextureDesc::Texture2D(source_dimensions.x, source_dimensions.y, 1, RmlTextureFormat));
        std::unique_ptr<RT64::RenderBuffer> upload_buffer = device_->createBuffer(
            RT64::RenderBufferDesc::UploadBuffer(aligned_row_pitch * source_dimensions.y));

        uint8_t* dst_bytes = reinterpret_cast<uint8_t*>(upload_buffer->map());
        for (int row = 0; row < source_dimensions.y; row++) {
            memcpy(dst_bytes + row * aligned_row_pitch, src_bytes + row * row_pitch, row_pitch);
        }
        upload_buffer->unmap();

        list_->barriers(
            RT64::RenderBarrierStage::ALL,
            RT64::RenderBufferBarrier(upload_buffer.get(), RT64::RenderBufferAccess::READ),
            RT64::RenderTextureBarrier(texture.get(), RT64::RenderTextureLayout::COPY_DEST));
        list_->copyTextureRegion(
            RT64::RenderTextureCopyLocation::Subresource(texture.get()),
            RT64::RenderTextureCopyLocation::PlacedFootprint(
                upload_buffer.get(), RmlTextureFormat, source_dimensions.x, source_dimensions.y, 1, aligned_row_width));
        list_->barriers(
            RT64::RenderBarrierStage::GRAPHICS,
            RT64::RenderTextureBarrier(texture.get(), RT64::RenderTextureLayout::SHADER_READ));

        // Keep staging memory alive until this command list is submitted.
        stale_buffers_.emplace_back(std::move(upload_buffer));

        std::unique_ptr<RT64::RenderDescriptorSet> set = texture_set_builder_->create(device_);
        set->setTexture(gTexture_descriptor_index, texture.get(), RT64::RenderTextureLayout::SHADER_READ);
        textures_.emplace(texture_handle, TextureHandle{ std::move(texture), std::move(set), true });
        return true;
    }

	void ReleaseTexture(Rml::TextureHandle texture) override {
        if (texture > 1) {
            // Textures #0 and #1 are reserved and should never be released.
            textures_.erase(texture);
        }
    }

    void SetTransform(const Rml::Matrix4f* transform) override {
        transform_ = transform ? *transform : Rml::Matrix4f::Identity();
        recalculate_mvp();
    }

    void recalculate_mvp() {
        mvp_ = projection_mtx_ * transform_;
    }

    void start(RT64::RenderCommandList* list, int image_width, int image_height) {
        list_ = list;

        // Drop staging buffers from the previous submitted frame before recording new uploads.
        stale_buffers_.clear();
        ensure_builtin_textures();
        upload_pending_textures();

        if (multisampling_.sampleCount > 1) {
            if (window_width_ != image_width || window_height_ != image_height) {
                screen_framebuffer_.reset();
                screen_texture_ = device_->createTexture(RT64::RenderTextureDesc::ColorTarget(image_width, image_height, SwapChainFormat));
                screen_texture_ms_ = device_->createTexture(RT64::RenderTextureDesc::ColorTarget(image_width, image_height, SwapChainFormat, multisampling_));
                const RT64::RenderTexture *color_attachment = screen_texture_ms_.get();
                screen_framebuffer_ = device_->createFramebuffer(RT64::RenderFramebufferDesc(&color_attachment, 1));
                screen_descriptor_set_->setTexture(0, screen_texture_.get(), RT64::RenderTextureLayout::SHADER_READ);
            }

            list_->setPipeline(pipeline_ms_.get());
        }
        else {
            list_->setPipeline(pipeline_.get());
        }

        list_->setGraphicsPipelineLayout(layout_.get());
        // Bind the set for descriptors that don't change across draws
        list_->setGraphicsDescriptorSet(sampler_set_.get(), 0);

        window_width_ = image_width;
        window_height_ = image_height;

        projection_mtx_ = Rml::Matrix4f::ProjectOrtho(0.0f, float(image_width), float(image_height), 0.0f, -10000, 10000);
        recalculate_mvp();

        // Reset per-frame geometry upload buffers.
        reset_dynamic_buffer(upload_buffer_);
        reset_dynamic_buffer(vertex_buffer_);
        reset_dynamic_buffer(index_buffer_);

        // Set an internal texture as the render target if MSAA is enabled.
        if (multisampling_.sampleCount > 1) {
            list->barriers(RT64::RenderBarrierStage::GRAPHICS, RT64::RenderTextureBarrier(screen_texture_ms_.get(), RT64::RenderTextureLayout::COLOR_WRITE));
            list->setFramebuffer(screen_framebuffer_.get());
            list->clearColor(0, RT64::RenderColor(0.0f, 0.0f, 0.0f, 0.0f));
        }
    }

    void end(RT64::RenderCommandList* list, RT64::RenderFramebuffer* framebuffer) {
        // Draw the texture were rendered the UI in to the swap chain framebuffer if MSAA is enabled.
        if (multisampling_.sampleCount > 1) {
            RT64::RenderTextureBarrier before_resolve_barriers[] = {
                RT64::RenderTextureBarrier(screen_texture_ms_.get(), RT64::RenderTextureLayout::RESOLVE_SOURCE),
                RT64::RenderTextureBarrier(screen_texture_.get(), RT64::RenderTextureLayout::RESOLVE_DEST)
            };

            list->barriers(RT64::RenderBarrierStage::COPY, before_resolve_barriers, uint32_t(std::size(before_resolve_barriers)));
            list->resolveTexture(screen_texture_.get(), screen_texture_ms_.get());
            list->barriers(RT64::RenderBarrierStage::GRAPHICS, RT64::RenderTextureBarrier(screen_texture_.get(), RT64::RenderTextureLayout::SHADER_READ));
            list->setFramebuffer(framebuffer);
            list->setPipeline(pipeline_.get());
            list->setGraphicsPipelineLayout(layout_.get());
            list->setGraphicsDescriptorSet(sampler_set_.get(), 0);
            list->setGraphicsDescriptorSet(screen_descriptor_set_.get(), 1);
            list->setScissors(RT64::RenderRect{ 0, 0, window_width_, window_height_ });
            RT64::RenderVertexBufferView vertex_view(screen_vertex_buffer_.get(), screen_vertex_buffer_size_);
            list->setVertexBuffers(0, &vertex_view, 1, &vertex_slot_);

            RmlPushConstants constants{
                .transform = Rml::Matrix4f::Identity(),
                .translation = Rml::Vector2f(0.0f, 0.0f)
            };

            list_->setGraphicsPushConstants(0, &constants);
            list->drawInstanced(3, 1, 0, 0);
        }

        end_dynamic_buffer(upload_buffer_);
        end_dynamic_buffer(vertex_buffer_);
        end_dynamic_buffer(index_buffer_);

        // Leave the swap chain unbound so the PRESENT transition is valid.
        list->setFramebuffer(nullptr);

        list_ = nullptr;
    }

    void queue_image_from_bytes_file(const std::string &src, const std::vector<char> &bytes) {
        // Width and height aren't used for file images, so set them to 0.
        image_from_bytes_queue.enqueue(ImageFromBytes{ .type = ImageType::File, .width = 0, .height = 0, .name = src, .bytes = bytes });
    }

    void queue_image_from_bytes_rgba32(const std::string &src, const std::vector<char> &bytes, uint32_t width, uint32_t height) {
        image_from_bytes_queue.enqueue(ImageFromBytes{ .type = ImageType::RGBA32, .width = width, .height = height, .name = src, .bytes = bytes });
    }

    void flush_image_from_bytes_queue() {
        ImageFromBytes image_from_bytes;
        while (image_from_bytes_queue.try_dequeue(image_from_bytes)) {
            // We can move the name into the map since the name in the actual entry is no longer needed.
            // After that, move the entry itself into the map.
            image_from_bytes_map.emplace(std::move(image_from_bytes.name), std::move(image_from_bytes));
        }
    }
};
} // namespace recompui

recompui::RmlRenderInterface_RT64::RmlRenderInterface_RT64() = default;
recompui::RmlRenderInterface_RT64::~RmlRenderInterface_RT64() = default;

void recompui::RmlRenderInterface_RT64::reset() {
    impl.reset();
}

void recompui::RmlRenderInterface_RT64::init(RT64::RenderInterface* interface, RT64::RenderDevice* device) {
    impl = std::make_unique<RmlRenderInterface_RT64_impl>(interface, device);
}

Rml::RenderInterface* recompui::RmlRenderInterface_RT64::get_rml_interface() {
    if (impl) {
        return impl->GetAdaptedInterface();
    }
    return nullptr;
}

void recompui::RmlRenderInterface_RT64::start(RT64::RenderCommandList* list, int image_width, int image_height) {
    assert(static_cast<bool>(impl));

    impl->start(list, image_width, image_height);
}

void recompui::RmlRenderInterface_RT64::end(RT64::RenderCommandList* list, RT64::RenderFramebuffer* framebuffer) {
    assert(static_cast<bool>(impl));

    impl->end(list, framebuffer);
}

void recompui::RmlRenderInterface_RT64::sync_upload_pending_textures() {
    assert(static_cast<bool>(impl));

    impl->sync_upload_pending_textures();
}

void recompui::RmlRenderInterface_RT64::queue_image_from_bytes_file(const std::string &src, const std::vector<char> &bytes) {
    assert(static_cast<bool>(impl));

    impl->queue_image_from_bytes_file(src, bytes);
}

void recompui::RmlRenderInterface_RT64::queue_image_from_bytes_rgba32(const std::string &src, const std::vector<char> &bytes, uint32_t width, uint32_t height) {
    assert(static_cast<bool>(impl));

    impl->queue_image_from_bytes_rgba32(src, bytes, width, height);
}
