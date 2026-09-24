#define GLFW_INCLUDE_VULKAN
#define csys_Implementation

#include <csys.h>
#include <cvulkan.h>

#include "ttf.c"

#define MB(x)       ((x) << 20)
#define LEN(x)      (sizeof((x))/sizeof(*(x)))

extern uint32_t g_spv_vs[];
extern uint32_t g_spv_vs_size;
extern uint32_t g_spv_fs[];
extern uint32_t g_spv_fs_size;

typedef struct State
{
    float       text_size;
    uint32_t    text_color;
    uint32_t    input_count;
    uint32_t    input[1 << 20];
} State;

typedef struct BDABuffer
{
    cvk_Buffer      buffer;
    VkDeviceMemory  memory;
    VkDeviceAddress address;
} BDABuffer;

typedef struct GPUBuffer
{
    cvk_Buffer      buffer;
    cvk_Memory      memory;
} GPUBuffer;

typedef struct DrawBuffer
{
    cvk_Buffer      buffer;
    VkDeviceMemory  memory;
    VkDeviceAddress address;
    void            *map;
} DrawBuffer;

typedef struct PushConstants
{
    vec2        view;
    uint32_t    color;
    float       size;
    uint64_t    band_ptr;
    uint64_t    point_ptr;
    uint64_t    glyph_ptr;
} PushConstants;

static uint32_t pack_draws(const State *state, DrawBuffer *buffer)
{
    float height = ttf_get_font_height(state->text_size);
    vec2 pos = VEC2(20.0f, height);
    uint32_t count = 0;

    GPUGlyph *ptr = buffer->map;
    for (uint32_t i = 0; i < state->input_count; ++i) {
        if (state->input[i] == '\n') {
            pos.x = 20.0f;
            pos.y += height;
            continue;
        }

        const Glyph *glyph = ttf_get_glyph(state->input[i]);
        if (glyph) {
            if (glyph->band_count) {
                float left = pos.x + glyph->bearing.x * state->text_size;
                float top = pos.y - glyph->bearing.y * state->text_size;

                ptr->band_count = glyph->band_count;
                ptr->band_offset = glyph->band_offset;
                ptr->pos = VEC2(left, top);
                ptr->min = glyph->min;
                ptr->max = glyph->max;
                ptr++;
                count++;
            }
            pos.x += glyph->advance * state->text_size;
        }
    }

    return count;
}

static cvk_Surface create_surface(VkInstance instance, GLFWwindow *window)
{
    VkSurfaceKHR result = VK_NULL_HANDLE;
    cvk_result_check(glfwCreateWindowSurface(instance, window, NULL, &result),
    "Failed to create the Vulkan Surface for the given GLFW window.");

    return result;
}

static cvk_pipeline_Graphics create_pipeline(cvk_device_Logical *dev, cvk_Allocator *allocator, VkFormat format)
{
    cvk_Shader vs = cvk_shader_create(&(cvk_shader_create_args){
        .device_logical = dev,
        .stage = cvk_shader_stage_Vertex,
        .code = &(cvk_SpirV){
            .len = g_spv_vs_size / 4,
            .ptr = g_spv_vs
        },
        .allocator = allocator
    });
    cvk_Shader fs = cvk_shader_create(&(cvk_shader_create_args){
        .device_logical = dev,
        .stage = cvk_shader_stage_Fragment,
        .code = &(cvk_SpirV){
            .len = g_spv_fs_size / 4,
            .ptr = g_spv_fs,
        },
        .allocator = allocator
    });
    VkPipelineShaderStageCreateInfo pipeline_stages[2] = {vs.stage, fs.stage};
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state = cvk_pipeline_state_dynamic_setup(2, dynamic_states);
    VkPipelineVertexInputStateCreateInfo vertex_input = cvk_pipeline_state_vertexInput_defaults();
    VkPipelineInputAssemblyStateCreateInfo input_assembly = cvk_pipeline_state_inputAssembly_defaults();
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewport_state = cvk_pipeline_state_viewport_defaults();
    VkPipelineRasterizationStateCreateInfo rasterization = cvk_pipeline_state_rasterization_defaults();
    VkPipelineMultisampleStateCreateInfo multisampling = cvk_pipeline_state_multisample_defaults();
    VkPipelineColorBlendAttachmentState blend_attachment = cvk_pipeline_state_colorBlend_attachment_defaults();
    VkPipelineColorBlendStateCreateInfo blend_state = cvk_pipeline_state_colorBlend_setup(&(cvk_pipeline_state_colorBlend_setup_args){
        .attachments_ptr = &blend_attachment
    });
    cvk_Rendering rendering = cvk_rendering_create(&(cvk_rendering_create_args){
        .color_format = format
    });

    cvk_pipeline_layout_create_args layout_args = {
        .device_logical = dev,
        .allocator = allocator,
        .pushConstants_len = 1,
        .pushConstants_ptr = &(VkPushConstantRange){
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,
            .size = 128
        }
    };

    cvk_pipeline_Graphics pipeline = cvk_pipeline_graphics_create(&(cvk_pipeline_graphics_create_args){
        .device_logical      = dev,
        .allocator           = allocator,
        .rendering           = &rendering,
        .stages              = &(cvk_pipeline_shaderStage_List){ .ptr = pipeline_stages, .len = 2 },
        .state_vertexInput   = &vertex_input,
        .state_inputAssembly = &input_assembly,
        .state_viewport      = &viewport_state,
        .state_rasterization = &rasterization,
        .state_multisample   = &multisampling,
        .state_colorBlend    = &blend_state,
        .state_dynamic       = &dynamic_state,
        .layout              = &layout_args
    });

    cvk_shader_destroy(&fs, dev, allocator);
    cvk_shader_destroy(&vs, dev, allocator);

    return pipeline;
}

BDABuffer create_bda_buffer(cvk_device_Physical *gpu,
                            cvk_device_Logical *dev,
                            cvk_Allocator *allocator,
                            size_t size)
{
    cvk_Buffer buffer = cvk_buffer_create(&(cvk_buffer_create_args){
        .device_physical = gpu,
        .device_logical = dev,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .size = size,
        .memory_flags = cvk_memory_DeviceLocal,
        .allocator = allocator
    });

    VkMemoryAllocateInfo alloc_info = (VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &(VkMemoryAllocateFlagsInfoKHR){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO_KHR,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR
        },
        .memoryTypeIndex = buffer.memory.kind,
        .allocationSize = buffer.memory.requirements.size
    };

    VkDeviceMemory memory;
    cvk_result_check(vkAllocateMemory(dev->ct, &alloc_info, allocator->gpu, &memory),
        "Failed to allocate a block of GPU memory.");
    cvk_result_check(vkBindBufferMemory(dev->ct, buffer.ct, memory, 0),
        "Failed to bind a block of GPU memory.");

    VkBufferDeviceAddressInfo address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer.ct
    };

    VkDeviceAddress address = vkGetBufferDeviceAddress(dev->ct, &address_info);

    BDABuffer result;
    result.buffer = buffer;
    result.memory = memory;
    result.address = address;

    return result;
}

static GPUBuffer create_staging(cvk_device_Physical *gpu, cvk_device_Logical *dev, cvk_Allocator *allocator, size_t size)
{
    cvk_Buffer buffer = cvk_buffer_create(&(cvk_buffer_create_args){
        .device_physical = gpu,
        .device_logical = dev,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .size = size,
        .memory_flags = cvk_memory_HostVisible | cvk_memory_HostCoherent,
        .allocator = allocator
    });
    cvk_Memory memory = cvk_memory_create(&(cvk_memory_create_args){
        .device_logical = dev,
        .kind = buffer.memory.kind,
        .size_alloc = buffer.memory.requirements.size,
        .persistent = cvk_true,
        .size_data = VK_WHOLE_SIZE,
        .allocator = allocator
    });
    cvk_buffer_bind(&buffer, &(cvk_buffer_bind_args){
        .device_logical = dev,
        .offset = 0,
        .memory = &memory
    });

    GPUBuffer result;
    result.buffer = buffer;
    result.memory = memory;

    return result;
}

DrawBuffer create_draw_buffer(cvk_device_Physical *gpu,
                              cvk_device_Logical *dev,
                              cvk_Allocator *allocator,
                              size_t size)
{
    cvk_Buffer buffer = cvk_buffer_create(&(cvk_buffer_create_args){
        .device_physical = gpu,
        .device_logical = dev,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .size = size,
        .memory_flags = cvk_memory_HostCoherent | cvk_memory_HostVisible,
        .allocator = allocator
    });

    VkMemoryAllocateInfo alloc_info = (VkMemoryAllocateInfo){
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &(VkMemoryAllocateFlagsInfoKHR){
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO_KHR,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR
        },
        .memoryTypeIndex = buffer.memory.kind,
        .allocationSize = buffer.memory.requirements.size
    };

    VkDeviceMemory memory;
    cvk_result_check(vkAllocateMemory(dev->ct, &alloc_info, allocator->gpu, &memory),
        "Failed to allocate a block of GPU memory.");
    cvk_result_check(vkBindBufferMemory(dev->ct, buffer.ct, memory, 0),
        "Failed to bind a block of GPU memory.");

    VkBufferDeviceAddressInfo address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer.ct
    };
    VkDeviceAddress address = vkGetBufferDeviceAddress(dev->ct, &address_info);

    void *map;
    cvk_result_check(vkMapMemory(dev->ct, memory, 0, VK_WHOLE_SIZE, 0, &map),
        "Failed to map a block of GPU memory.");

    DrawBuffer result;
    result.buffer = buffer;
    result.memory = memory;
    result.address = address;
    result.map = map;

    return result;
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    State *state = glfwGetWindowUserPointer(window);
    if (action == GLFW_PRESS || action == GLFW_REPEAT) {
        switch (key) {
            case GLFW_KEY_UP: {
                state->text_size += 1.0f;
                break;
            }

            case GLFW_KEY_DOWN: {
                state->text_size -= 1.0f;
                if (state->text_size <= 8.0f)
                    state->text_size = 8.0f;
                break;
            }

            case GLFW_KEY_BACKSPACE: {
                if (state->input_count)
                    state->input_count--;
                break;
            }

            case GLFW_KEY_ENTER: {
                if (state->input_count < LEN(state->input))
                    state->input[state->input_count++] = '\n';
                break;
            }

            default: break;
        }
    }
}

static void character_callback(GLFWwindow* window, unsigned int codepoint)
{
    State *state = glfwGetWindowUserPointer(window);
    if (state->input_count < LEN(state->input))
        state->input[state->input_count++] = codepoint;
}

static void record_flush(cvk_command_Buffer *cb, GPUBuffer *staging, BDABuffer *points, BDABuffer *bands)
{
    size_t points_size = ttf_point_buffer_offset * sizeof(vec2);
    size_t bands_size = ttf_band_buffer_offset * sizeof(uint32_t);
    memcpy(staging->memory.data, ttf_point_buffer, points_size);
    memcpy((char *)staging->memory.data + points_size, ttf_band_buffer, bands_size);

    cvk_command_buffer_reset(cb, cvk_false);
    cvk_command_buffer_begin(cb);

    VkBufferCopy regions[2] = {
        {
            .srcOffset = 0,
            .dstOffset = 0,
            .size = points_size
        },
        {
            .srcOffset = points_size,
            .dstOffset = 0,
            .size = bands_size
        }
    };

    vkCmdCopyBuffer(cb->ct, staging->buffer.ct, points->buffer.ct, 1, regions);
    vkCmdCopyBuffer(cb->ct, staging->buffer.ct, bands->buffer.ct, 1, regions + 1);

    cvk_command_buffer_end(cb);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stdout, "Usage: %s path/to/font/file.ttf\n", argv[0]);
        return 1;
    }

    const char *font_path = argv[1];
    ttf_init(font_path);

    State *state = xmalloc(sizeof(*state));
    state->text_size = 20.0f;
    state->text_color = 0xffffffff;
    state->input_count = 0;

#ifdef __linux__
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
#endif
    csys_System system = csys_init(csys_init_defaults());
    glfwSetKeyCallback(system.window.ct, key_callback);
    glfwSetWindowUserPointer(system.window.ct, state);
    glfwSetCharCallback(system.window.ct, character_callback);

    cvk_instance_extensions_Required extensions = {0};
    extensions.system.ptr = glfwGetRequiredInstanceExtensions((uint32_t*)&extensions.system.len);
    cvk_Instance instance = cvk_instance_create(&(cvk_instance_create_args){ .extensions = extensions });
    cvk_Surface surface = create_surface(instance.ct, system.window.ct);

    cvk_device_features_Required features = {0};
    features.user.v1_2.bufferDeviceAddress = VK_TRUE;

    cvk_device_Physical gpu = cvk_device_physical_create(&(cvk_device_physical_create_args){
        .instance = &instance,
        .surface = surface,
        .features = &features
    });

    cvk_device_Queue queue = cvk_device_queue_create_noContext(&(cvk_device_queue_create_args){
        .instance = &instance,
        .device = &gpu,
        .id = gpu.queueFamilies.graphics,
        .priority = 1.0f
    });

    cvk_device_Logical dev = cvk_device_logical_create(&(cvk_device_logical_create_args){
        .physical = &gpu,
        .queue = &queue,
        .features = &features,
        .allocator = &instance.allocator
    });
    cvk_device_queue_create_context(&queue, &dev);

    cvk_device_Swapchain swapchain = cvk_device_swapchain_create(&(cvk_device_swapchain_create_args){
        .device_physical = &gpu,
        .device_logical = &dev,
        .surface = surface,
        .size = (cvk_Size2D){system.window.width, system.window.height},
        .allocator = &instance.allocator
    });

    cvk_pipeline_Graphics pipeline = create_pipeline(&dev, &instance.allocator, swapchain.cfg.imageFormat);
    cvk_command_Pool command_pool = cvk_command_pool_create(&(cvk_command_pool_create_args){
        .device_logical = &dev,
        .queueID = gpu.queueFamilies.graphics,
        .flags = cvk_command_pool_Reset,
        .allocator = &instance.allocator
    });

    cvk_command_Buffer command_buffer[2];
    cvk_Semaphore image_available[2];
    cvk_Fence frames_pending[2];
    for (int i = 0; i < 2; ++i) {
        command_buffer[i] = cvk_command_buffer_allocate(&(cvk_command_buffer_allocate_args){
          .device_logical = &dev,
          .command_pool   = &command_pool
        });
        image_available[i] = cvk_semaphore_create(&dev, &instance.allocator);
        frames_pending[i] = cvk_fence_create(&dev, cvk_true, &instance.allocator);
    }

    cvk_command_Buffer staging_cb = cvk_command_buffer_allocate(&(cvk_command_buffer_allocate_args){
      .device_logical = &dev,
      .command_pool   = &command_pool
    });

    BDABuffer point_buffer = create_bda_buffer(&gpu, &dev, &instance.allocator, MB(3));
    BDABuffer band_buffer = create_bda_buffer(&gpu, &dev, &instance.allocator, MB(3));

    DrawBuffer draw_buffers[2] = {
        create_draw_buffer(&gpu, &dev, &instance.allocator, MB(1)),
        create_draw_buffer(&gpu, &dev, &instance.allocator, MB(1)),
    };

    GPUBuffer staging_buffer = create_staging(&gpu, &dev, &instance.allocator, MB(10));

    uint32_t input_buffer_len = 0;
    uint32_t *input_buffer = xmalloc(MB(1));

    //
    // Loop
    //

    int frame_id = 0;
    while (!csys_close(&system)) {
        csys_update(&system);

        cvk_fence_wait(&frames_pending[frame_id], &dev);
        cvk_fence_reset(&frames_pending[frame_id], &dev);

        // Generate draws for the frame
        DrawBuffer *draw_buffer = draw_buffers + frame_id;
        uint32_t draw_count = pack_draws(state, draw_buffer);

        // Flush buffers
        if (ttf_buffers_dirty) {
            record_flush(&staging_cb, &staging_buffer, &point_buffer, &band_buffer);
            cvk_device_queue_submit(&queue, &(cvk_device_queue_submit_args){
              .command_buffer = &staging_cb
            });
            vkDeviceWaitIdle(dev.ct);
            ttf_buffers_dirty = false;
        }

        cvk_size const image_id = cvk_device_swapchain_nextImageID(&swapchain, &(cvk_device_swapchain_nextImageID_args){
            .device_logical = &dev,
            .semaphore      = &image_available[frame_id]
        });

        cvk_command_Buffer *cb = &command_buffer[frame_id];
        cvk_command_buffer_reset(cb, cvk_false);
        cvk_command_buffer_begin(cb);

        cvk_command_image_handle_transition(cb, swapchain.images.ptr[image_id].ct, &(cvk_image_transition_args){
            .layout_old = VK_IMAGE_LAYOUT_UNDEFINED,
            .layout_new = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .access_src = 0,
            .access_trg = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .stage_src  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            .stage_trg  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
        });

        float width = (float)swapchain.cfg.imageExtent.width;
        float height = (float)swapchain.cfg.imageExtent.height;
        cvk_Rendering rendering = cvk_rendering_create(&(cvk_rendering_create_args){
            .color_format = swapchain.cfg.imageFormat,
            .extent       = swapchain.cfg.imageExtent,
            .color_load   = VK_ATTACHMENT_LOAD_OP_CLEAR
        });
        rendering.color.view = swapchain.images.ptr[image_id].view;

        cvk_command_pipeline_graphics_bind(cb, &pipeline);
        cvk_command_viewport_set(cb, &(VkViewport){
            .width    = width,
            .height   = height,
            .maxDepth = 1.0f
        });
        cvk_command_scissor_set(cb, &(VkRect2D){
          .offset = (VkOffset2D){.x = 0, .y = 0},
          .extent = swapchain.cfg.imageExtent
        });

        cvk_command_rendering_begin(cb, &(cvk_command_rendering_begin_args){ .rendering = &rendering });
        if (draw_count) {
            PushConstants pc = {
                .view = VEC2(width, height),
                .color = state->text_color,
                .size = state->text_size,
                .band_ptr = band_buffer.address,
                .point_ptr = point_buffer.address,
                .glyph_ptr = draw_buffer->address
            };

            cvk_command_constants_push(cb, &(cvk_command_constants_push_args){
                .size = sizeof(pc),
                .data = &pc,
                .stage = VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,
                .pipeline_layout = &pipeline.layout,
                .offset = 0
            });

            cvk_command_draw(cb, &(cvk_command_draw_args){
                .elements_len = 4,
                .instance_len = draw_count
            });
        }
        cvk_command_rendering_end(cb);

        cvk_command_image_handle_transition(cb, swapchain.images.ptr[image_id].ct, &(cvk_image_transition_args){
          .layout_old = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .layout_new = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
          .access_src = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .access_trg = 0,
          .stage_src  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          .stage_trg  = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        });

        cvk_command_buffer_end(cb);
        cvk_device_queue_submit(&queue, &(cvk_device_queue_submit_args){
          .command_buffer   = cb,
          .semaphore_wait   = &image_available[frame_id],
          .semaphore_signal = &swapchain.images.ptr[image_id].finished,
          .fence            = &frames_pending[frame_id],
        });
        cvk_device_swapchain_present(&swapchain, image_id, &queue);

        frame_id = (frame_id + 1) % 2;
    }

    return 0;
}
