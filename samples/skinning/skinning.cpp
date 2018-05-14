/*
* Brokkr framework
*
* Copyright(c) 2017 by Ferran Sole
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files(the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions :
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include "application.h"
#include "render.h"
#include "window.h"
#include "image.h"
#include "mesh.h"
#include "maths.h"
#include "timer.h"
#include "camera.h"

using namespace bkk;
using namespace maths;

static const char* gVertexShaderSource = R"(
  #version 440 core

  layout(location = 0) in vec3 aPosition;
  layout(location = 1) in vec3 aNormal;
  layout(location = 2) in vec2 aTexCoord;
  layout(location = 3) in vec4 aBonesWeight;
  layout(location = 4) in vec4 aBonesId;

  layout(binding = 0) uniform UNIFORMS
  {
    mat4 modelView;
    mat4 modelViewProjection;
  }uniforms;

  layout(binding = 1) buffer BONESTX
  {
    mat4 bones[];
  }bonesTx;

  out OUTPUT
  {
    vec3 normalViewSpace;
    vec3 lightViewSpace;
    vec2 uv;
  }output_;

  void main(void)
  {
    mat4 transform = bonesTx.bones[int(aBonesId[0])] * aBonesWeight[0] +
                     bonesTx.bones[int(aBonesId[1])] * aBonesWeight[1] +
                     bonesTx.bones[int(aBonesId[2])] * aBonesWeight[2] +
                     bonesTx.bones[int(aBonesId[3])] * aBonesWeight[3];

    output_.normalViewSpace = normalize((mat4(inverse(transpose(uniforms.modelView * transform))) * vec4(aNormal,0.0)).xyz);
    output_.lightViewSpace = normalize((uniforms.modelView * vec4(normalize(vec3(0.0,0.0,1.0)),0.0)).xyz);
    output_.uv = aTexCoord;

    gl_Position = uniforms.modelViewProjection * transform * vec4(aPosition,1.0);
  }
)";

static const char* gFragmentShaderSource = R"(
  #version 440 core

  in INPUT
  {
    vec3 normalViewSpace;
    vec3 lightViewSpace;
    vec2 uv;
  }input_;

  layout (binding = 2) uniform sampler2D uTexture;
  
  layout(location = 0) out vec4 color;
  
  void main(void)
  {
    float diffuse = max(dot(normalize(input_.lightViewSpace), normalize(input_.normalViewSpace)), 0.0);
    color = texture( uTexture,input_.uv) * diffuse;
  }
)";


class skinning_sample_t : public application_t
{
public:
  skinning_sample_t()
  :application_t("Skinning", 600u, 600u, 3u),
   camera_(25.0f, vec2(0.8f, 0.0f), 0.01f),
   projectionTx_( perspectiveProjectionMatrix(1.5f, 1.0f, 1.0f, 1000.0f) ),
   modelTx_( createTransform(vec3(0.0,-17.0,0.0), VEC3_ONE, QUAT_UNIT) )
  {
    render::context_t& context = getRenderContext();
            
    //Create uniform buffer
    mat4 matrices[2];
    matrices[0] = modelTx_ * camera_.view_;
    matrices[1] = matrices[0] * projectionTx_;
    render::gpuBufferCreate(context, render::gpu_buffer_t::usage::UNIFORM_BUFFER,
                            render::gpu_memory_type_e::HOST_VISIBLE_COHERENT,
                            (void*)&matrices, sizeof(matrices) ,
                            nullptr, &globalUnifomBuffer_);

    //Create geometry and animator    
    mesh::createFromFile(context, "../resources/mannequin/mannequin.fbx", mesh::EXPORT_ALL, nullptr, 0u, &mesh_);
    mesh::animatorCreate(context, mesh_, 0u, 1.0f, &animator_);

    //Load texture
    bkk::image::image2D_t image = {};
    if (!bkk::image::load("../resources/mannequin/diffuse.jpg", false, &image))
    {
      printf("Error loading texture\n");
    }
    else
    {
      //Create the texture
      bkk::render::texture2DCreate(context, &image, 1, bkk::render::texture_sampler_t(), &texture_);
      bkk::image::unload(&image);
    }

    //Create pipeline and descriptor set layouts
    render::descriptor_binding_t bindings[3] = { render::descriptor_binding_t{ render::descriptor_t::type::UNIFORM_BUFFER, 0u, render::descriptor_t::stage::VERTEX },
                                                 render::descriptor_binding_t{ render::descriptor_t::type::STORAGE_BUFFER, 1u, render::descriptor_t::stage::VERTEX }, //Bones transforms
                                                 render::descriptor_binding_t{ render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 2u, render::descriptor_t::stage::FRAGMENT} 
                                               };

    render::descriptorSetLayoutCreate(context, bindings, 3u, &descriptorSetLayout_);
    render::pipelineLayoutCreate(context, &descriptorSetLayout_, 1u, nullptr, 0u, &pipelineLayout_);

    //Create descriptor set
    render::descriptorPoolCreate(context, 1u,
      render::combined_image_sampler_count(1u),
      render::uniform_buffer_count(1u),
      render::storage_buffer_count(1u),
      render::storage_image_count(0u),
      &descriptorPool_);

    render::descriptor_t descriptors[3] = { render::getDescriptor(globalUnifomBuffer_), render::getDescriptor(animator_.buffer_), render::getDescriptor( texture_) };
    render::descriptorSetCreate(context, descriptorPool_, descriptorSetLayout_, descriptors, &descriptorSet_);

    //Create pipeline
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::VERTEX_SHADER, gVertexShaderSource, &vertexShader_);
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::FRAGMENT_SHADER, gFragmentShaderSource, &fragmentShader_);
    bkk::render::graphics_pipeline_t::description_t pipelineDesc;
    pipelineDesc.viewPort_ = { 0.0f, 0.0f, (float)context.swapChain_.imageWidth_, (float)context.swapChain_.imageHeight_, 0.0f, 1.0f };
    pipelineDesc.scissorRect_ = { { 0,0 },{ context.swapChain_.imageWidth_,context.swapChain_.imageHeight_ } };
    pipelineDesc.blendState_.resize(1);
    pipelineDesc.blendState_[0].colorWriteMask = 0xF;
    pipelineDesc.blendState_[0].blendEnable = VK_FALSE;
    pipelineDesc.cullMode_ = VK_CULL_MODE_BACK_BIT;
    pipelineDesc.depthTestEnabled_ = true;
    pipelineDesc.depthWriteEnabled_ = true;
    pipelineDesc.depthTestFunction_ = VK_COMPARE_OP_LESS_OR_EQUAL;
    pipelineDesc.vertexShader_ = vertexShader_;
    pipelineDesc.fragmentShader_ = fragmentShader_;
    render::graphicsPipelineCreate(context, context.swapChain_.renderPass_, 0u, mesh_.vertexFormat_, pipelineLayout_, pipelineDesc, &pipeline_);

    buildCommandBuffers();
  }

  void onQuit()
  {    
    render::context_t& context = getRenderContext();

    mesh::destroy(context, &mesh_);
    mesh::animatorDestroy(context, &animator_);
       
    render::shaderDestroy(context, &vertexShader_);
    render::shaderDestroy(context, &fragmentShader_);

    render::pipelineLayoutDestroy(context, &pipelineLayout_);
    render::graphicsPipelineDestroy(context, &pipeline_);
    render::descriptorSetLayoutDestroy(context, &descriptorSetLayout_);
    render::descriptorSetDestroy(context, &descriptorSet_);    
    render::descriptorPoolDestroy(context, &descriptorPool_);
    render::gpuBufferDestroy(context, nullptr, &globalUnifomBuffer_);    
  }
  
  void render()
  {
    render::context_t& context = getRenderContext();

    //Update uniform buffer
    mat4 matrices[2];
    matrices[0] = modelTx_ * camera_.view_;
    matrices[1] = matrices[0] * projectionTx_;
    render::gpuBufferUpdate(context, (void*)&matrices, 0, sizeof(matrices), &globalUnifomBuffer_);
    
    //Update animator
    mesh::animatorUpdate(context, getTimeDelta(), &animator_);

    //Render frame
    render::presentFrame(&context);
  }
  
  void onResize(u32 width, u32 height) 
  {
    buildCommandBuffers();
    projectionTx_ = perspectiveProjectionMatrix(1.5f, width / (float)height, 1.0f, 1000.0f);
  }

  void onKeyEvent(window::key_e key, bool pressed) 
  {
    if (pressed)
    {
      switch (key)
      {
        case window::key_e::KEY_UP:
        case 'w':
        {
          camera_.Move(-1.0f);
          break;
        }
        case window::key_e::KEY_DOWN:
        case 's':
        {
          camera_.Move(1.0f);
          break;
        }

        default:
          break;
      }
    }
  }

  void onMouseMove(const vec2& mousePos, const vec2& mouseDeltaPos, bool buttonPressed)
  {
    if (buttonPressed)
    {
      camera_.Rotate(mouseDeltaPos.x, mouseDeltaPos.y);
    }
  }

  void buildCommandBuffers()
  {
    render::context_t& context = getRenderContext();
    
    VkClearValue clearValues[2];
    clearValues[0].color = { { 0.0f, 0.0f, 1.0f, 1.0f } };

    clearValues[1].depthStencil = { 1.0f,0 };
    const VkCommandBuffer* commandBuffers;
    uint32_t count = render::getPresentationCommandBuffers(context, &commandBuffers);
    for (uint32_t i(0); i<count; ++i)
    {
      render::beginPresentationCommandBuffer(context, i, clearValues);
      bkk::render::graphicsPipelineBind(commandBuffers[i], pipeline_);
      bkk::render::descriptorSetBindForGraphics(commandBuffers[i], pipelineLayout_, 0, &descriptorSet_, 1u);
      mesh::draw(commandBuffers[i], mesh_);
      render::endPresentationCommandBuffer(context, i);
    }
  }

private:
  
  render::gpu_buffer_t globalUnifomBuffer_;

  mesh::mesh_t mesh_;
  mesh::skeletal_animator_t animator_;  
  render::texture_t texture_;

  render::pipeline_layout_t pipelineLayout_;
  render::descriptor_set_layout_t descriptorSetLayout_;

  render::descriptor_pool_t descriptorPool_;  
  render::descriptor_set_t descriptorSet_;

  render::graphics_pipeline_t pipeline_;
  render::shader_t vertexShader_;
  render::shader_t fragmentShader_;

  camera::orbiting_camera_t camera_;
  maths::mat4 projectionTx_;
  maths::mat4 modelTx_;
};

//Entry point
int main()
{
  skinning_sample_t sample;
  sample.loop();
  return 0;
}
