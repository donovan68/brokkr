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

#include "render.h"
#include "window.h"
#include "mesh.h"
#include "maths.h"
#include "timer.h"
#include "../utility.h"
#include "transform-manager.h"
#include "packed-freelist.h"

using namespace bkk;
using namespace maths;

static const char* gGeometryPassVertexShaderSource = {
  "#version 440 core\n \
  layout(location = 0) in vec3 aPosition;\n \
  layout(location = 1) in vec3 aNormal;\n \
  layout(location = 2) in vec2 aUV;\n \
  layout (set = 0, binding = 0) uniform SCENE\n \
  {\n \
    mat4 worldToView;\n \
    mat4 viewToWorld;\n\
    mat4 projection;\n \
    mat4 projectionInverse;\n \
    vec4 imageSize;\n \
  }scene;\n \
  layout(set = 1, binding = 0) uniform MODEL\n \
  {\n \
    mat4 transform;\n \
  }model;\n \
  layout(location = 0) out vec3 normalViewSpace;\n \
  layout(location = 1) out vec2 uv;\n \
  void main(void)\n \
  {\n \
    mat4 modelView = scene.worldToView * model.transform;\n \
    gl_Position = scene.projection * modelView * vec4(aPosition,1.0);\n \
    normalViewSpace = normalize((transpose( inverse( modelView) ) * vec4(aNormal,0.0)).xyz);\n \
    uv = aUV;\n \
  }\n"
};


static const char* gGeometryPassFragmentShaderSource = {
  "#version 440 core\n \
  layout(set = 2, binding = 0) uniform MATERIAL\n \
  {\n \
    vec3 albedo;\n \
    float metallic;\n\
    vec3 F0;\n \
    float roughness;\n \
  }material;\n \
  layout(location = 0) out vec4 RT0;\n \
  layout(location = 1) out vec4 RT1;\n \
  layout(location = 2) out vec4 RT2;\n \
  layout(location = 0) in vec3 normalViewSpace;\n \
  layout(location = 1) in vec2 uv;\n \
  void main(void)\n \
  {\n \
    RT0 = vec4( material.albedo, material.roughness);\n \
    RT1 = vec4(normalize(normalViewSpace), gl_FragCoord.z);\n \
    RT2 = vec4( material.F0, material.metallic);\n \
  }\n"
};

static const char* gPointLightPassVertexShaderSource = {
  "#version 440 core\n \
  layout(location = 0) in vec3 aPosition;\n \
  layout (set = 0, binding = 0) uniform SCENE\n \
  {\n \
    mat4 worldToView;\n \
    mat4 viewToWorld;\n\
    mat4 projection;\n \
    mat4 projectionInverse;\n \
    vec4 imageSize;\n \
  }scene;\n \
  layout (set = 2, binding = 0) uniform LIGHT\n \
  {\n \
   vec4 position;\n \
   vec3 color;\n \
   float radius;\n \
  }light;\n \
  layout(location = 0) out vec3 lightPositionVS;\n\
  void main(void)\n \
  {\n \
    mat4 viewProjection = scene.projection * scene.worldToView;\n \
    vec4 vertexPosition =  vec4( aPosition*light.radius+light.position.xyz, 1.0 );\n\
    gl_Position = viewProjection * vertexPosition;\n\
    lightPositionVS = (scene.worldToView * light.position).xyz;\n\
  }\n"
};


static const char* gPointLightPassFragmentShaderSource = {
  "#version 440 core\n \
  layout (set = 0, binding = 0) uniform SCENE\n \
  {\n \
    mat4 worldToView;\n \
    mat4 viewToWorld;\n\
    mat4 projection;\n \
    mat4 projectionInverse;\n \
    vec4 imageSize;\n \
  }scene;\n \
  layout (set = 2, binding = 0) uniform LIGHT\n \
  {\n \
   vec4 position;\n \
   vec3 color;\n \
   float radius;\n \
  }light;\n \
  layout(set = 1, binding = 0) uniform sampler2D RT0;\n \
  layout(set = 1, binding = 1) uniform sampler2D RT1;\n \
  layout(set = 1, binding = 2) uniform sampler2D RT2;\n \
  layout(location = 0) in vec3 lightPositionVS;\n\
  const float PI = 3.14159265359;\n\
  layout(location = 0) out vec4 result;\n \
  vec3 ViewSpacePositionFromDepth(vec2 uv, float depth)\n\
  {\n\
    vec3 clipSpacePosition = vec3(uv* 2.0 - 1.0, depth);\n\
    vec4 viewSpacePosition = scene.projectionInverse * vec4(clipSpacePosition,1.0);\n\
    return(viewSpacePosition.xyz / viewSpacePosition.w);\n\
  }\n\
  vec3 fresnelSchlick(float cosTheta, vec3 F0)\n\
  {\n\
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);\n\
  }\n\
  float DistributionGGX(vec3 N, vec3 H, float roughness)\n\
  {\n\
    float a = roughness*roughness;\n\
    float a2 = a*a;\n\
    float NdotH = max(dot(N, H), 0.0);\n\
    float NdotH2 = NdotH*NdotH;\n\
    float nom = a2;\n\
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);\n\
    denom = PI * denom * denom;\n\
    return nom / denom;\n\
  }\n\
  float GeometrySchlickGGX(float NdotV, float roughness)\n\
  {\n\
    float r = (roughness + 1.0);\n\
    float k = (r*r) / 8.0;\n\
    float nom = NdotV;\n\
    float denom = NdotV * (1.0 - k) + k;\n\
    return nom / denom;\n\
  }\n\
  float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)\n\
  {\n\
    float NdotV = max(dot(N, V), 0.0);\n\
    float NdotL = max(dot(N, L), 0.0);\n\
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);\n\
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);\n\
    return ggx1 * ggx2;\n\
  }\n\
  void main(void)\n \
  {\n \
    vec2 uv = gl_FragCoord.xy * scene.imageSize.zw;\n\
    vec4 RT0Value = texture(RT0, uv);\n \
    vec3 albedo = RT0Value.xyz;\n\
    float roughness = RT0Value.w;\n\
    vec4 RT1Value = texture(RT1, uv);\n \
    vec3 N = normalize(RT1Value.xyz); \n \
    float depth = RT1Value.w;\n\
    vec4 RT2Value = texture(RT2, uv);\n \
    vec3 positionVS = ViewSpacePositionFromDepth( uv,depth );\n\
    vec3 L = normalize( lightPositionVS-positionVS );\n\
    vec3 F0 = RT2Value.xyz;\n \
    float metallic = RT2Value.w;\n\
    vec3 V = -normalize(positionVS);\n\
    vec3 H = normalize(V + L);\n\
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);\n \
    float NDF = DistributionGGX(N, H, roughness);\n\
    float G = GeometrySmith(N, V, L, roughness);\n\
    vec3 kS = F;\n\
    vec3 kD = vec3(1.0) - kS;\n\
    kD *= 1.0 - metallic;\n\
    vec3 nominator = NDF * G * F;\n\
    float denominator = 4 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001;\n\
    vec3 specular = nominator / denominator;\n\
    float lightDistance    = length(lightPositionVS - positionVS);\n\
    float attenuation = 1.0 - clamp( lightDistance / light.radius, 0.0, 1.0);\n\
    attenuation *= attenuation;\n\
    float NdotL =  max( 0.0, dot( N, L ) );\n \
    vec3 color = (kD * albedo / PI + specular) * (light.color*attenuation) * NdotL;\n\
    color = color / (color + vec3(1.0));\n\
    color = pow(color, vec3(1.0 / 2.2));\n\
    result = vec4(color,1.0);\n\
  }\n"
};

static const char* gDirectionalLightPassVertexShaderSource = {
  "#version 440 core\n \
  layout(location = 0) in vec3 aPosition;\n \
  layout(location = 1) in vec2 aUV;\n \
  layout (set = 0, binding = 0) uniform SCENE\n \
  {\n \
    mat4 worldToView;\n \
    mat4 viewToWorld;\n\
    mat4 projection;\n \
    mat4 projectionInverse;\n \
    vec4 imageSize;\n \
  }scene;\n \
  layout (set = 2, binding = 0) uniform LIGHT\n \
  {\n \
   vec4 position;\n \
   vec3 color;\n \
   float radius;\n \
  }light;\n \
  void main(void)\n \
  {\n \
    gl_Position = vec4(aPosition,1.0);\n \
  }\n"
};

static const char* gDirectionalLightPassFragmentShaderSource = {
  "#version 440 core\n \
  layout (set = 0, binding = 0) uniform SCENE\n \
  {\n \
    mat4 worldToView;\n \
    mat4 viewToWorld;\n\
    mat4 projection;\n \
    mat4 projectionInverse;\n \
    vec4 imageSize;\n \
  }scene;\n \
  layout (set = 2, binding = 0) uniform LIGHT\n \
  {\n \
    vec4 direction;\n \
    vec4 color;\n \
    mat4 worldToLightClipSpace; \n\
    vec4 shadowMapSize; \n \
  }light;\n \
  layout(set = 1, binding = 0) uniform sampler2D RT0;\n \
  layout(set = 1, binding = 1) uniform sampler2D RT1;\n \
  layout(set = 1, binding = 2) uniform sampler2D RT2;\n \
  layout(set = 1, binding = 3) uniform sampler2D shadowMapRT0;\n \
  layout(set = 1, binding = 4) uniform sampler2D shadowMapRT1;\n \
  layout(set = 1, binding = 5) uniform sampler2D shadowMapRT2;\n \
  const float PI = 3.14159265359;\n\
  layout(location = 0) out vec4 result;\n \
  vec3 ViewSpacePositionFromDepth(vec2 uv, float depth)\n\
  {\n\
    vec3 clipSpacePosition = vec3(uv* 2.0 - 1.0, depth);\n\
    vec4 viewSpacePosition = scene.projectionInverse * vec4(clipSpacePosition,1.0);\n\
    return(viewSpacePosition.xyz / viewSpacePosition.w);\n\
  }\n\
  vec3 fresnelSchlick(float cosTheta, vec3 F0)\n\
  {\n\
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);\n\
  }\n\
  float DistributionGGX(vec3 N, vec3 H, float roughness)\n\
  {\n\
    float a = roughness*roughness;\n\
    float a2 = a*a;\n\
    float NdotH = max(dot(N, H), 0.0);\n\
    float NdotH2 = NdotH*NdotH;\n\
    float nom = a2;\n\
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);\n\
    denom = PI * denom * denom;\n\
    return nom / denom;\n\
  }\n\
  float GeometrySchlickGGX(float NdotV, float roughness)\n\
  {\n\
    float r = (roughness + 1.0);\n\
    float k = (r*r) / 8.0;\n\
    float nom = NdotV;\n\
    float denom = NdotV * (1.0 - k) + k;\n\
    return nom / denom;\n\
  }\n\
  float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)\n\
  {\n\
    float NdotV = max(dot(N, V), 0.0);\n\
    float NdotL = max(dot(N, L), 0.0);\n\
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);\n\
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);\n\
    return ggx1 * ggx2;\n\
  }\n\
  void main(void)\n \
  {\n \
    vec2 uv = gl_FragCoord.xy * scene.imageSize.zw;\n\
    vec4 RT0Value = texture(RT0, uv);\n \
    vec3 albedo = RT0Value.xyz;\n\
    float roughness = RT0Value.w;\n\
    vec4 RT1Value = texture(RT1, uv);\n \
    vec3 N = normalize(RT1Value.xyz); \n \
    float depth = RT1Value.w;\n\
    vec4 RT2Value = texture(RT2, uv);\n \
    vec3 positionVS = ViewSpacePositionFromDepth( uv,depth );\n\
    vec3 L = normalize( (scene.worldToView * vec4(light.direction.xyz,0.0)).xyz );\n\
    vec3 F0 = RT2Value.xyz;\n \
    float metallic = RT2Value.w;\n\
    vec3 V = -normalize(positionVS);\n\
    vec3 H = normalize(V + L);\n\
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);\n \
    float NDF = DistributionGGX(N, H, roughness);\n\
    float G = GeometrySmith(N, V, L, roughness);\n\
    vec3 kS = F;\n\
    vec3 kD = vec3(1.0) - kS;\n\
    kD *= 1.0 - metallic;\n\
    vec3 nominator = NDF * G * F;\n\
    float denominator = 4 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001;\n\
    vec3 specular = nominator / denominator;\n\
    float NdotL =  max( 0.0, dot( N, L ) );\n \
    vec3 diffuseColor = albedo / PI;\n\
    vec3 ambientColor = light.color.a * diffuseColor;\n\
    vec4 postionInLigthClipSpace = light.worldToLightClipSpace * scene.viewToWorld * vec4(positionVS, 1.0 );\n\
    postionInLigthClipSpace.xyz /= postionInLigthClipSpace.w;\n\
    postionInLigthClipSpace.xy = 0.5 * postionInLigthClipSpace.xy + 0.5;\n\
    ivec2 shadowMapUV = ivec2( postionInLigthClipSpace.xy * light.shadowMapSize.xy );\n\
    float bias = 0.005;//0.0005*tan(acos(NdotL));\n\
    float attenuation = 0.0;\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2( 0, 0), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2( 1, 0), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2(-1, 0), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2( 0, 1), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2( 0,-1), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2( 1, 1), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2(-1, 1), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2(-1,-1), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2( 1,-1), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation /= 9.0;\n\
    vec3 color = (kD * diffuseColor + specular) * (light.color.rgb * attenuation) * NdotL + ambientColor;\n\
    color = color / (color + vec3(1.0));\n\
    color = pow(color, vec3(1.0 / 2.2));\n\
    result = vec4(color,1.0);\n\
  }\n"
};

static const char* gDirectionalLightPassGIFragmentShaderSource = {
  "#version 440 core\n \
  layout (set = 0, binding = 0) uniform SCENE\n \
  {\n \
    mat4 worldToView;\n \
    mat4 viewToWorld;\n\
    mat4 projection;\n \
    mat4 projectionInverse;\n \
    vec4 imageSize;\n \
  }scene;\n \
  layout (set = 2, binding = 0) uniform LIGHT\n \
  {\n \
    vec4 direction;\n \
    vec4 color;\n \
    mat4 worldToLightClipSpace; \n\
    vec4 shadowMapSize; \n \
    vec3 padding;\n\
    float sampleCount;\n\
    vec4 samples[400];\n\
  }light;\n \
  layout(set = 1, binding = 0) uniform sampler2D RT0;\n \
  layout(set = 1, binding = 1) uniform sampler2D RT1;\n \
  layout(set = 1, binding = 2) uniform sampler2D RT2;\n \
  layout(set = 1, binding = 3) uniform sampler2D shadowMapRT0;\n \
  layout(set = 1, binding = 4) uniform sampler2D shadowMapRT1;\n \
  layout(set = 1, binding = 5) uniform sampler2D shadowMapRT2;\n \
  const float PI = 3.14159265359;\n\
  layout(location = 0) out vec4 result;\n \
  vec3 ViewSpacePositionFromDepth(in vec2 uv, in float depth)\n\
  {\n\
    vec3 clipSpacePosition = vec3(uv* 2.0 - 1.0, depth);\n\
    vec4 viewSpacePosition = scene.projectionInverse * vec4(clipSpacePosition,1.0);\n\
    return(viewSpacePosition.xyz / viewSpacePosition.w);\n\
  }\n\
  vec3 fresnelSchlick(in float cosTheta, in vec3 F0)\n\
  {\n\
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);\n\
  }\n\
  float DistributionGGX(in vec3 N, in vec3 H, in float roughness)\n\
  {\n\
    float a = roughness*roughness;\n\
    float a2 = a*a;\n\
    float NdotH = max(dot(N, H), 0.0);\n\
    float NdotH2 = NdotH*NdotH;\n\
    float nom = a2;\n\
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);\n\
    denom = PI * denom * denom;\n\
    return nom / denom;\n\
  }\n\
  float GeometrySchlickGGX(in float NdotV, in float roughness)\n\
  {\n\
    float r = (roughness + 1.0);\n\
    float k = (r*r) / 8.0;\n\
    float nom = NdotV;\n\
    float denom = NdotV * (1.0 - k) + k;\n\
    return nom / denom;\n\
  }\n\
  float GeometrySmith(in vec3 N, in vec3 V, in vec3 L, in float roughness)\n\
  {\n\
    float NdotV = max(dot(N, V), 0.0);\n\
    float NdotL = max(dot(N, L), 0.0);\n\
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);\n\
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);\n\
    return ggx1 * ggx2;\n\
  }\n\
  vec3 sampleIndirectLight(in vec3 positionWS, in vec3 normalWS, in ivec2 uv )\n\
  {\n\
    vec3 indirectRadiance = vec3(0.0,0.0,0.0);\n\
    for( uint i = 0; i<400; ++i )\n\
    {\n\
      vec3 shadowMapNormal =  normalize( texelFetch( shadowMapRT0, clamp(ivec2(uv + light.samples[i].xy), ivec2(0,0),ivec2(light.shadowMapSize.x,light.shadowMapSize.y)), 0 ).yzw );\n\
      vec3 shadowMapPosition = texelFetch( shadowMapRT1, clamp(ivec2(uv + light.samples[i].xy), ivec2(0,0),ivec2(light.shadowMapSize.x,light.shadowMapSize.y)), 0 ).xyz;\n\
      vec3 shadowMapRadiance = texelFetch( shadowMapRT2, clamp(ivec2(uv + light.samples[i].xy), ivec2(0,0),ivec2(light.shadowMapSize.x,light.shadowMapSize.y)), 0 ).xyz;\n\
      vec3 L = normalize( shadowMapPosition-positionWS );\n\
      float distance = length(shadowMapPosition-positionWS);\n\
      float G = max(0.0, dot(normalWS, L)) * max(0.0,dot(shadowMapNormal,-L)) / distance*distance;\n\
      indirectRadiance +=  G * shadowMapRadiance ;\n\
    }\n\
    return indirectRadiance * 0.005;\n\
  }\n\
  void main(void)\n \
  {\n \
    vec2 uv = gl_FragCoord.xy * scene.imageSize.zw;\n\
    vec4 RT0Value = texture(RT0, uv);\n \
    vec3 albedo = RT0Value.xyz;\n\
    float roughness = RT0Value.w;\n\
    vec4 RT1Value = texture(RT1, uv);\n \
    vec3 N = normalize(RT1Value.xyz); \n \
    float depth = RT1Value.w;\n\
    vec4 RT2Value = texture(RT2, uv);\n \
    vec3 positionVS = ViewSpacePositionFromDepth( uv,depth );\n\
    vec3 L = normalize( (scene.worldToView * vec4(light.direction.xyz,0.0)).xyz );\n\
    vec3 F0 = RT2Value.xyz;\n \
    float metallic = RT2Value.w;\n\
    vec3 V = -normalize(positionVS);\n\
    vec3 H = normalize(V + L);\n\
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);\n \
    float NDF = DistributionGGX(N, H, roughness);\n\
    float G = GeometrySmith(N, V, L, roughness);\n\
    vec3 kS = F;\n\
    vec3 kD = vec3(1.0) - kS;\n\
    kD *= 1.0 - metallic;\n\
    vec3 nominator = NDF * G * F;\n\
    float denominator = 4 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.001;\n\
    vec3 specular = nominator / denominator;\n\
    float NdotL =  max( 0.0, dot( N, L ) );\n \
    vec3 diffuseColor = albedo / PI;\n\
    vec3 ambientColor = light.color.a * diffuseColor;\n\
    vec4 postionInLigthClipSpace = light.worldToLightClipSpace * scene.viewToWorld * vec4(positionVS, 1.0 );\n\
    postionInLigthClipSpace.xyz /= postionInLigthClipSpace.w;\n\
    postionInLigthClipSpace.xy = 0.5 * postionInLigthClipSpace.xy + 0.5;\n\
    ivec2 shadowMapUV = ivec2( postionInLigthClipSpace.xy * light.shadowMapSize.xy );\n\
    float bias = 0.005;//0.0005*tan(acos(NdotL));\n\
    float attenuation = 0.0;\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2( 0, 0), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2( 1, 0), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2(-1, 0), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2( 0, 1), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2( 0,-1), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2( 1, 1), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2(-1, 1), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2(-1,-1), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation += step( 0.5, float((texelFetch( shadowMapRT0, shadowMapUV+ivec2( 1,-1), 0).r + bias) > postionInLigthClipSpace.z ));\n\
    attenuation /= 9.0;\n\
    vec3 color = (kD * diffuseColor + specular) * (light.color.rgb * attenuation) * NdotL + ambientColor;\n\
    //if( attenuation < 0.5 )\n\
    {\n\
      vec3 positionWS = (scene.viewToWorld * vec4(positionVS, 1.0 )).xyz;\n\
      vec3 normalWS = normalize((transpose( inverse( scene.viewToWorld) ) * vec4(RT1Value.xyz,0.0)).xyz);\n \
      color += sampleIndirectLight(positionWS, normalWS, shadowMapUV);\n\
    }\n\
    color = color / (color + vec3(1.0));\n\
    color = pow(color, vec3(1.0 / 2.2));\n\
    result = vec4(color,1.0);\n\
  }\n"
};

static const char* gShadowPassVertexShaderSource = {
  "#version 440 core\n \
  layout(location = 0) in vec3 aPosition;\n \
  layout(location = 1) in vec3 aNormal;\n \
  layout(location = 2) in vec2 aUV;\n \
  layout (set = 0, binding = 0) uniform LIGHT\n \
  {\n \
    vec4 direction;\n \
    vec4 color;\n \
    mat4 worldToLightClipSpace; \n\
    vec4 shadowMapSize; \n \
  }light;\n \
  layout(set = 1, binding = 0) uniform MODEL\n \
  {\n \
    mat4 transform;\n \
  }model;\n \
  layout(set = 2, binding = 1) uniform sampler2D diffuseMap;\n \
  layout( location = 0 ) out vec3 positionWS;\n\
  layout( location = 1 ) out vec3 normalWS;\n\
  layout( location = 2 ) out vec2 UV;\n\
  void main(void)\n \
  {\n \
    gl_Position =  light.worldToLightClipSpace * model.transform * vec4(aPosition,1.0);\n \
    normalWS = normalize((transpose( inverse( model.transform) ) * vec4(aNormal,0.0)).xyz);\n \
    positionWS = ( model.transform * vec4(aPosition, 1.0) ).xyz;\n\
    UV = aUV;\n\
  }\n"
};

static const char* gShadowPassFragmentShaderSource = {
  "#version 440 core\n \
  layout(location = 0) out vec4 RT0;\n \
  layout(location = 1) out vec4 RT1;\n \
  layout(location = 2) out vec4 RT2;\n \
  layout (set = 0, binding = 0) uniform LIGHT\n \
  {\n \
    vec4 direction;\n \
    vec4 color;\n \
    mat4 worldToLightClipSpace; \n\
    vec4 shadowMapSize; \n \
  }light;\n \
  layout(set = 2, binding = 0) uniform MATERIAL\n \
  {\n \
    vec3 albedo;\n \
    float metallic;\n\
    vec3 F0;\n \
    float roughness;\n \
  }material;\n \
  layout( location = 0 ) in vec3 positionWS;\n\
  layout( location = 1 ) in vec3 normalWS;\n\
  layout( location = 2 ) in vec2 UV;\n\
  void main(void)\n \
  {\n \
    RT0 = vec4( gl_FragCoord.z, normalize( normalWS ) );\n \
    RT1 = vec4(positionWS, 1.0);\n\
    vec3 radiance = max( 0.0, dot( normalize(light.direction.xyz), normalize(normalWS) ) ) * material.albedo;\n\
    RT2 = vec4( radiance, 0.0);\n\
  }\n"
};

static const char* gPresentationVertexShaderSource = {
  "#version 440 core\n \
  layout(location = 0) in vec3 aPosition;\n \
  layout(location = 1) in vec2 aTexCoord;\n \
  layout(location = 0) out vec2 uv;\n \
  void main(void)\n \
  {\n \
    gl_Position = vec4(aPosition,1.0);\n \
    uv = aTexCoord;\n \
  }\n"
};

static const char* gPresentationFragmentShaderSource = {
  "#version 440 core\n \
  layout(location = 0) in vec2 uv;\n  \
  layout (set = 0, binding = 0) uniform sampler2D uTexture;\n \
  layout(location = 0) out vec4 color;\n \
  void main(void)\n \
  {\n \
    color = texture(uTexture, uv);\n \
  }\n"
};

struct scene_t
{
  struct point_light_t
  {
    struct uniforms_t
    {
      maths::vec4 position_;
      maths::vec3 color_;
      float radius_;
    };

    uniforms_t uniforms_;
    render::gpu_buffer_t ubo_;
    render::descriptor_set_t descriptorSet_;
  };

  struct directional_light_t
  {
    struct uniforms_t
    {
      maths::vec4 direction_;
      maths::vec4 color_;             //RGB is light color, A is ambient
      maths::mat4 worldToClipSpace_;  //Transforms points from world space to light clip space
      maths::vec4 shadowMapSize_;
      maths::vec3 padding_;
      float sampleCount_ = 400;
      maths::vec4 samples_[400];
    };


    uniforms_t uniforms_;
    maths::vec3 position_;  //For shadow map rendering
    maths::mat4 viewProjection_;
    render::gpu_buffer_t ubo_;
    render::descriptor_set_t descriptorSet_;
  };

  struct material_t
  {
    struct uniforms_t
    {
      vec3 albedo_;
      float metallic_;
      vec3 F0_;
      float roughness_;
    };

    uniforms_t uniforms_;
    render::gpu_buffer_t ubo_;
    render::texture_t diffuseMap_;
    render::descriptor_set_t descriptorSet_;
  };

  struct object_t
  {
    bkk::handle_t mesh_;
    bkk::handle_t material_;
    bkk::handle_t transform_;
    render::gpu_buffer_t ubo_;
    render::descriptor_set_t descriptorSet_;
  };

  struct scene_uniforms_t
  {
    mat4 worldToViewMatrix_;
    mat4 viewToWorldMatrix_;
    mat4 projectionMatrix_;
    mat4 projectionInverseMatrix_;
    vec4 imageSize_;
  };


  void load(const char* url)
  {
    //Meshes
    mesh::mesh_t* mesh = nullptr;
    uint32_t meshCount = mesh::createFromFile(*context_, url, mesh::EXPORT_ALL, &allocator_, &mesh);
    std::vector<bkk::handle_t> meshHandles(meshCount);
    for (u32 i(0); i < meshCount; ++i)
    {
      meshHandles[i] = mesh_.add(mesh[i]);
    }
    delete[] mesh;

    //Materials
    mesh::material_t* materials;
    uint32_t* materialIndex;
    uint32_t materialCount = mesh::loadMaterials(url, &materialIndex, &materials);
    std::vector<bkk::handle_t> materialHandles(materialCount);

    std::string modelPath = url;
    modelPath = modelPath.substr(0u, modelPath.find_last_of("/") + 1);
    for (u32 i(0); i < materialCount; ++i)
    {
      std::string diffuseMapPath = "";
      if (!materials[i].diffuseMap_.empty())
      {
        diffuseMapPath = modelPath + materials[i].diffuseMap_;
      }
      materialHandles[i] = addMaterial(materials[i].kd_, 0.0f, vec3(0.1f, 0.1f, 0.1f), 0.5f, diffuseMapPath);
    }
    delete[] materials;

    //Objects
    for (u32 i(0); i < meshCount; ++i)
    {
      addObject(meshHandles[i], materialHandles[materialIndex[i]], maths::computeTransform(maths::vec3(0.0f, 0.0f, 0.0f), maths::vec3(0.001f, 0.001f, 0.001f), maths::QUAT_UNIT));
    }

    delete[] materialIndex;
  }

  bkk::handle_t addMesh(const char* url)
  {
    mesh::mesh_t mesh;
    mesh::createFromFile(*context_, url, mesh::EXPORT_ALL, &allocator_, 0u, &mesh);
    return mesh_.add(mesh);
  }

  bkk::handle_t addMaterial(const vec3& albedo, float metallic, const vec3& F0, float roughness, std::string diffuseMap)
  {
    //Create uniform buffer and descriptor set
    material_t material = {};
    material.uniforms_.albedo_ = albedo;
    material.uniforms_.metallic_ = metallic;
    material.uniforms_.F0_ = F0;
    material.uniforms_.roughness_ = roughness;
    render::gpuBufferCreate(*context_, render::gpu_buffer_t::usage::UNIFORM_BUFFER,
      &material.uniforms_, sizeof(material_t::uniforms_t),
      &allocator_, &material.ubo_);

    render::descriptor_t descriptor = render::getDescriptor(material.ubo_);
    render::descriptorSetCreate(*context_, descriptorPool_, materialDescriptorSetLayout_, &descriptor, &material.descriptorSet_);
    return material_.add(material);
  }

  bkk::handle_t addObject(bkk::handle_t meshId, bkk::handle_t materialId, const maths::mat4& transform)
  {
    bkk::handle_t transformId = transformManager_.createTransform(transform);

    //Create uniform buffer and descriptor set
    render::gpu_buffer_t ubo;
    render::gpuBufferCreate(*context_, render::gpu_buffer_t::usage::UNIFORM_BUFFER,
      nullptr, sizeof(mat4),
      &allocator_, &ubo);

    object_t object = { meshId, materialId, transformId, ubo };
    render::descriptor_t descriptor = render::getDescriptor(object.ubo_);
    render::descriptorSetCreate(*context_, descriptorPool_, objectDescriptorSetLayout_, &descriptor, &object.descriptorSet_);
    return object_.add(object);
  }


  void addDirectionalLight(const maths::vec3& position, const maths::vec3& direction, const maths::vec3& color, float ambient)
  {
    if (directionalLight_ == nullptr)
    {
      directionalLight_ = new directional_light_t;

      vec3 lightDirection = normalize(direction);
      directionalLight_->uniforms_.direction_ = maths::vec4(lightDirection, 0.0f);
      directionalLight_->uniforms_.color_ = vec4(color, ambient);

      mat4 lightViewMatrix;
      quat orientation(vec3(0.0f, 0.0f, 1.0f), lightDirection);
      mat4 lightModelMatrix = maths::computeTransform(position, VEC3_ONE, orientation);
      computeInverse(lightModelMatrix, lightViewMatrix);

      directionalLight_->uniforms_.worldToClipSpace_ = lightViewMatrix * computeOrthographicProjectionMatrix(-1.0f, 1.0f, 1.0f, -1.0f, 0.01f, 2.0f);
      directionalLight_->uniforms_.shadowMapSize_ = vec4((float)shadowMapSize_, (float)shadowMapSize_, 1.0f / (float)shadowMapSize_, 1.0f / (float)shadowMapSize_);

      //Generate sampling pattern
      for (uint32_t i(0); i < 400; ++i)
      {
        float e1 = rand();// ((double)rand() / (RAND_MAX));
        float e2 = rand();// ((double)rand() / (RAND_MAX));
        float rMax = 0.3;
        directionalLight_->uniforms_.samples_[i] = vec4(rMax * e1 * sinf(2.0f * M_PI * e2), rMax * e1 * cosf(2.0f * M_PI * e2), e1*e1, 0.0f);
      }
      //Create uniform buffer and descriptor set
      render::gpuBufferCreate(*context_, render::gpu_buffer_t::usage::UNIFORM_BUFFER,
        &directionalLight_->uniforms_, sizeof(directional_light_t::uniforms_t),
        &allocator_, &directionalLight_->ubo_);

      render::descriptor_t descriptor = render::getDescriptor(directionalLight_->ubo_);
      render::descriptorSetCreate(*context_, descriptorPool_, lightDescriptorSetLayout_, &descriptor, &directionalLight_->descriptorSet_);

      initializeShadowPass(*context_);
    }
  }

  bkk::handle_t addPointLight(const maths::vec3& position, float radius, const maths::vec3& color)
  {
    point_light_t light;

    light.uniforms_.position_ = maths::vec4(position, 1.0);
    light.uniforms_.color_ = color;
    light.uniforms_.radius_ = radius;
    //Create uniform buffer and descriptor set
    render::gpuBufferCreate(*context_, render::gpu_buffer_t::usage::UNIFORM_BUFFER,
      &light.uniforms_, sizeof(point_light_t::uniforms_t),
      &allocator_, &light.ubo_);

    render::descriptor_t descriptor = render::getDescriptor(light.ubo_);
    render::descriptorSetCreate(*context_, descriptorPool_, lightDescriptorSetLayout_, &descriptor, &light.descriptorSet_);
    return pointLight_.add(light);
  }

  point_light_t* getLight(bkk::handle_t lightId)
  {
    return pointLight_.get(lightId);
  }

  void initializeShadowPass(render::context_t& context)
  {
    shadowRenderPass_ = {};
    render::render_pass_t::attachment_t shadowAttachments[4];
    shadowAttachments[0].format_ = shadowMapRT0_.format_;
    shadowAttachments[0].initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    shadowAttachments[0].finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    shadowAttachments[0].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    shadowAttachments[0].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    shadowAttachments[0].samples_ = VK_SAMPLE_COUNT_1_BIT;

    shadowAttachments[1].format_ = shadowMapRT1_.format_;
    shadowAttachments[1].initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    shadowAttachments[1].finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    shadowAttachments[1].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    shadowAttachments[1].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    shadowAttachments[1].samples_ = VK_SAMPLE_COUNT_1_BIT;

    shadowAttachments[2].format_ = shadowMapRT2_.format_;
    shadowAttachments[2].initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    shadowAttachments[2].finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    shadowAttachments[2].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    shadowAttachments[2].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    shadowAttachments[2].samples_ = VK_SAMPLE_COUNT_1_BIT;

    shadowAttachments[3].format_ = depthStencilBuffer_.format_;
    shadowAttachments[3].initialLayout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    shadowAttachments[3].finallLayout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    shadowAttachments[3].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    shadowAttachments[3].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    shadowAttachments[3].samples_ = VK_SAMPLE_COUNT_1_BIT;

    render::render_pass_t::subpass_t shadowPass;
    shadowPass.colorAttachmentIndex_.push_back(0);
    shadowPass.colorAttachmentIndex_.push_back(1);
    shadowPass.colorAttachmentIndex_.push_back(2);
    shadowPass.depthStencilAttachmentIndex_ = 3;

    //Dependency chain for layout transitions
    render::render_pass_t::subpass_dependency_t shadowDependencies[2];
    shadowDependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    shadowDependencies[0].dstSubpass = 0;
    shadowDependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    shadowDependencies[0].dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    shadowDependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    shadowDependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    shadowDependencies[1].srcSubpass = 0;
    shadowDependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    shadowDependencies[1].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    shadowDependencies[1].dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    shadowDependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    shadowDependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    render::renderPassCreate(context, 4u, shadowAttachments, 1u, &shadowPass, 2u, shadowDependencies, &shadowRenderPass_);

    //Create frame buffer
    VkImageView shadowFbAttachment[4] = { shadowMapRT0_.imageView_,  shadowMapRT1_.imageView_,  shadowMapRT2_.imageView_, shadowPassDepthStencilBuffer.imageView_ };
    render::frameBufferCreate(context, shadowMapSize_, shadowMapSize_, shadowRenderPass_, shadowFbAttachment, &shadowFrameBuffer_);

    //Create shadow pipeline layout
    render::descriptor_binding_t binding = { render::descriptor_t::type::UNIFORM_BUFFER, 0, render::descriptor_t::stage::VERTEX | render::descriptor_t::stage::FRAGMENT };
    render::descriptorSetLayoutCreate(context, 1u, &binding, &shadowGlobalsDescriptorSetLayout_);
    render::descriptor_t descriptor = render::getDescriptor(directionalLight_->ubo_);
    render::descriptorSetCreate(context, descriptorPool_, shadowGlobalsDescriptorSetLayout_, &descriptor, &shadowGlobalsDescriptorSet_);
    render::descriptor_set_layout_t shadowDescriptorSetLayouts[3] = { shadowGlobalsDescriptorSetLayout_, objectDescriptorSetLayout_, materialDescriptorSetLayout_ };
    render::pipelineLayoutCreate(context, 3, shadowDescriptorSetLayouts, &shadowPipelineLayout_);

    //Create shadow pipeline
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::VERTEX_SHADER, gShadowPassVertexShaderSource, &shadowVertexShader_);
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::FRAGMENT_SHADER, gShadowPassFragmentShaderSource, &shadowFragmentShader_);
    bkk::render::graphics_pipeline_t::description_t shadowPipelineDesc = {};
    shadowPipelineDesc.viewPort_ = { 0.0f, 0.0f, (float)shadowMapSize_, (float)shadowMapSize_, 0.0f, 1.0f };
    shadowPipelineDesc.scissorRect_ = { { 0,0 },{ shadowMapSize_, shadowMapSize_ } };
    shadowPipelineDesc.blendState_.resize(3);
    shadowPipelineDesc.blendState_[0].colorWriteMask = 0xF;
    shadowPipelineDesc.blendState_[0].blendEnable = VK_FALSE;
    shadowPipelineDesc.blendState_[1].colorWriteMask = 0xF;
    shadowPipelineDesc.blendState_[1].blendEnable = VK_FALSE;
    shadowPipelineDesc.blendState_[2].colorWriteMask = 0xF;
    shadowPipelineDesc.blendState_[2].blendEnable = VK_FALSE;
    shadowPipelineDesc.cullMode_ = VK_CULL_MODE_NONE;
    shadowPipelineDesc.depthTestEnabled_ = true;
    shadowPipelineDesc.depthWriteEnabled_ = true;
    shadowPipelineDesc.depthTestFunction_ = VK_COMPARE_OP_LESS_OR_EQUAL;
    shadowPipelineDesc.vertexShader_ = shadowVertexShader_;
    shadowPipelineDesc.fragmentShader_ = shadowFragmentShader_;
    render::graphicsPipelineCreate(context, shadowRenderPass_.handle_, 0u, vertexFormat_, shadowPipelineLayout_, shadowPipelineDesc, &shadowPipeline_);
  }

  void initializeOffscreenPass(render::context_t& context, const uvec2& size)
  {
    //Semaphore to indicate rendering has completed
    VkSemaphoreCreateInfo semaphoreCreateInfo = {};
    semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(context.device_, &semaphoreCreateInfo, nullptr, &renderComplete_);
    vkCreateSemaphore(context.device_, &semaphoreCreateInfo, nullptr, &shadowPassComplete_);

    //Create render targets 
    render::texture2DCreate(context, size.x, size.y, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &gBufferRT0_);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &gBufferRT0_);
    render::texture2DCreate(context, size.x, size.y, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &gBufferRT1_);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &gBufferRT1_);
    render::texture2DCreate(context, size.x, size.y, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &gBufferRT2_);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &gBufferRT2_);
    render::texture2DCreate(context, size.x, size.y, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &finalImage_);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &finalImage_);
    render::depthStencilBufferCreate(context, size.x, size.y, &depthStencilBuffer_);

    //Shadow map
    render::texture2DCreate(context, shadowMapSize_, shadowMapSize_, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &shadowMapRT0_);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &shadowMapRT0_);
    render::texture2DCreate(context, shadowMapSize_, shadowMapSize_, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &shadowMapRT1_);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &shadowMapRT1_);
    render::texture2DCreate(context, shadowMapSize_, shadowMapSize_, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT, render::texture_sampler_t(), &shadowMapRT2_);
    bkk::render::textureChangeLayoutNow(context, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, &shadowMapRT2_);
    render::depthStencilBufferCreate(context, shadowMapSize_, shadowMapSize_, &shadowPassDepthStencilBuffer);

    //Create offscreen render pass (GBuffer + light subpasses)
    renderPass_ = {};
    render::render_pass_t::attachment_t attachments[5];
    attachments[0].format_ = gBufferRT0_.format_;
    attachments[0].initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].samples_ = VK_SAMPLE_COUNT_1_BIT;

    attachments[1].format_ = gBufferRT1_.format_;;
    attachments[1].initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].samples_ = VK_SAMPLE_COUNT_1_BIT;

    attachments[2].format_ = gBufferRT2_.format_;;
    attachments[2].initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[2].finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[2].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[2].samples_ = VK_SAMPLE_COUNT_1_BIT;

    attachments[3].format_ = finalImage_.format_;
    attachments[3].initialLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[3].finallLayout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[3].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[3].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[3].samples_ = VK_SAMPLE_COUNT_1_BIT;

    attachments[4].format_ = depthStencilBuffer_.format_;
    attachments[4].initialLayout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[4].finallLayout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[4].storeOp_ = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[4].loadOp_ = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[4].samples_ = VK_SAMPLE_COUNT_1_BIT;

    render::render_pass_t::subpass_t subpasses[2];
    subpasses[0].colorAttachmentIndex_.push_back(0);
    subpasses[0].colorAttachmentIndex_.push_back(1);
    subpasses[0].colorAttachmentIndex_.push_back(2);
    subpasses[0].depthStencilAttachmentIndex_ = 4;

    subpasses[1].inputAttachmentIndex_.push_back(0);
    subpasses[1].inputAttachmentIndex_.push_back(1);
    subpasses[1].inputAttachmentIndex_.push_back(2);
    subpasses[1].colorAttachmentIndex_.push_back(3);

    //Dependency chain for layout transitions
    render::render_pass_t::subpass_dependency_t dependencies[4];
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    dependencies[1].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].dstSubpass = 1;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    dependencies[2].srcSubpass = 0;
    dependencies[2].dstSubpass = 1;
    dependencies[2].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[2].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[2].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    dependencies[3].srcSubpass = 1;
    dependencies[3].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[3].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependencies[3].dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    dependencies[3].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[3].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    render::renderPassCreate(context, 5u, attachments, 2u, subpasses, 4u, dependencies, &renderPass_);

    //Create frame buffer
    VkImageView fbAttachment[5] = { gBufferRT0_.imageView_, gBufferRT1_.imageView_, gBufferRT2_.imageView_, finalImage_.imageView_, depthStencilBuffer_.imageView_ };
    render::frameBufferCreate(context, size.x, size.y, renderPass_, fbAttachment, &frameBuffer_);

    //Create descriptorSets layouts
    render::descriptor_binding_t objectBindings = { render::descriptor_t::type::UNIFORM_BUFFER, 0, render::descriptor_t::stage::VERTEX };
    render::descriptorSetLayoutCreate(context, 1u, &objectBindings, &objectDescriptorSetLayout_);

    render::descriptor_binding_t materialBindings = { render::descriptor_t::type::UNIFORM_BUFFER, 0, render::descriptor_t::stage::FRAGMENT };
    render::descriptorSetLayoutCreate(context, 1u, &materialBindings, &materialDescriptorSetLayout_);

    //Create gBuffer pipeline layout
    render::descriptor_set_layout_t descriptorSetLayouts[3] = { globalsDescriptorSetLayout_, objectDescriptorSetLayout_, materialDescriptorSetLayout_ };
    render::pipelineLayoutCreate(context, 3, descriptorSetLayouts, &gBufferPipelineLayout_);

    //Create geometry pass pipeline
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::VERTEX_SHADER, gGeometryPassVertexShaderSource, &gBuffervertexShader_);
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::FRAGMENT_SHADER, gGeometryPassFragmentShaderSource, &gBufferfragmentShader_);
    bkk::render::graphics_pipeline_t::description_t pipelineDesc = {};
    pipelineDesc.viewPort_ = { 0.0f, 0.0f, (float)context.swapChain_.imageWidth_, (float)context.swapChain_.imageHeight_, 0.0f, 1.0f };
    pipelineDesc.scissorRect_ = { { 0,0 },{ context.swapChain_.imageWidth_,context.swapChain_.imageHeight_ } };
    pipelineDesc.blendState_.resize(3);
    pipelineDesc.blendState_[0].colorWriteMask = 0xF;
    pipelineDesc.blendState_[0].blendEnable = VK_FALSE;
    pipelineDesc.blendState_[1].colorWriteMask = 0xF;
    pipelineDesc.blendState_[1].blendEnable = VK_FALSE;
    pipelineDesc.blendState_[2].colorWriteMask = 0xF;
    pipelineDesc.blendState_[2].blendEnable = VK_FALSE;
    pipelineDesc.cullMode_ = VK_CULL_MODE_BACK_BIT;
    pipelineDesc.depthTestEnabled_ = true;
    pipelineDesc.depthWriteEnabled_ = true;
    pipelineDesc.depthTestFunction_ = VK_COMPARE_OP_LESS_OR_EQUAL;
    pipelineDesc.vertexShader_ = gBuffervertexShader_;
    pipelineDesc.fragmentShader_ = gBufferfragmentShader_;
    render::graphicsPipelineCreate(context, renderPass_.handle_, 0u, vertexFormat_, gBufferPipelineLayout_, pipelineDesc, &gBufferPipeline_);

    //Create light pass descriptorSet layouts
    render::descriptor_binding_t bindings[6];
    bindings[0] = { render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 0, render::descriptor_t::stage::FRAGMENT };
    bindings[1] = { render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 1, render::descriptor_t::stage::FRAGMENT };
    bindings[2] = { render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 2, render::descriptor_t::stage::FRAGMENT };
    bindings[3] = { render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 3, render::descriptor_t::stage::FRAGMENT };
    bindings[4] = { render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 4, render::descriptor_t::stage::FRAGMENT };
    bindings[5] = { render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 5, render::descriptor_t::stage::FRAGMENT };
    render::descriptorSetLayoutCreate(context, 6u, bindings, &lightPassTexturesDescriptorSetLayout_);

    render::descriptor_binding_t lightBindings = { render::descriptor_t::type::UNIFORM_BUFFER, 0, render::descriptor_t::stage::VERTEX | render::descriptor_t::stage::FRAGMENT };
    render::descriptorSetLayoutCreate(context, 1u, &lightBindings, &lightDescriptorSetLayout_);

    //Create descriptor sets for light pass (GBuffer textures)
    render::descriptor_t descriptors[6];
    descriptors[0] = render::getDescriptor(gBufferRT0_);
    descriptors[1] = render::getDescriptor(gBufferRT1_);
    descriptors[2] = render::getDescriptor(gBufferRT2_);
    descriptors[3] = render::getDescriptor(shadowMapRT0_);
    descriptors[4] = render::getDescriptor(shadowMapRT1_);
    descriptors[5] = render::getDescriptor(shadowMapRT2_);
    render::descriptorSetCreate(context, descriptorPool_, lightPassTexturesDescriptorSetLayout_, descriptors, &lightPassTexturesDescriptorSet_);

    //Create light pass pipeline layout
    render::descriptor_set_layout_t lightPassDescriptorSetLayouts[3] = { globalsDescriptorSetLayout_, lightPassTexturesDescriptorSetLayout_, lightDescriptorSetLayout_ };
    render::pipelineLayoutCreate(context, 3u, lightPassDescriptorSetLayouts, &lightPipelineLayout_);

    //Create point light pass pipeline
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::VERTEX_SHADER, gPointLightPassVertexShaderSource, &pointLightVertexShader_);
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::FRAGMENT_SHADER, gPointLightPassFragmentShaderSource, &pointLightFragmentShader_);
    bkk::render::graphics_pipeline_t::description_t lightPipelineDesc = {};
    lightPipelineDesc.viewPort_ = { 0.0f, 0.0f, (float)context.swapChain_.imageWidth_, (float)context.swapChain_.imageHeight_, 0.0f, 1.0f };
    lightPipelineDesc.scissorRect_ = { { 0,0 },{ context.swapChain_.imageWidth_,context.swapChain_.imageHeight_ } };
    lightPipelineDesc.blendState_.resize(1);
    lightPipelineDesc.blendState_[0].colorWriteMask = 0xF;
    lightPipelineDesc.blendState_[0].blendEnable = VK_TRUE;
    lightPipelineDesc.blendState_[0].colorBlendOp = VK_BLEND_OP_ADD;
    lightPipelineDesc.blendState_[0].alphaBlendOp = VK_BLEND_OP_ADD;
    lightPipelineDesc.blendState_[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    lightPipelineDesc.blendState_[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    lightPipelineDesc.blendState_[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    lightPipelineDesc.blendState_[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    lightPipelineDesc.cullMode_ = VK_CULL_MODE_FRONT_BIT;
    lightPipelineDesc.depthTestEnabled_ = false;
    lightPipelineDesc.depthWriteEnabled_ = false;
    lightPipelineDesc.vertexShader_ = pointLightVertexShader_;
    lightPipelineDesc.fragmentShader_ = pointLightFragmentShader_;
    render::graphicsPipelineCreate(context, renderPass_.handle_, 1u, sphereMesh_.vertexFormat_, lightPipelineLayout_, lightPipelineDesc, &pointLightPipeline_);

    //Create directional light pass pipeline
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::VERTEX_SHADER, gDirectionalLightPassVertexShaderSource, &directionalLightVertexShader_);
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::FRAGMENT_SHADER, gDirectionalLightPassFragmentShaderSource, &directionalLightFragmentShader_);
    lightPipelineDesc.cullMode_ = VK_CULL_MODE_BACK_BIT;
    lightPipelineDesc.vertexShader_ = directionalLightVertexShader_;
    lightPipelineDesc.fragmentShader_ = directionalLightFragmentShader_;
    render::graphicsPipelineCreate(context, renderPass_.handle_, 1u, fullScreenQuad_.vertexFormat_, lightPipelineLayout_, lightPipelineDesc, &directionalLightPipeline_);

    //Create Global illumination (Reflective shadowmaps) directional light pass pipeline
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::FRAGMENT_SHADER, gDirectionalLightPassGIFragmentShaderSource, &directionalLightGIFragmentShader_);
    lightPipelineDesc.cullMode_ = VK_CULL_MODE_BACK_BIT;
    lightPipelineDesc.vertexShader_ = directionalLightVertexShader_;
    lightPipelineDesc.fragmentShader_ = directionalLightGIFragmentShader_;
    render::graphicsPipelineCreate(context, renderPass_.handle_, 1u, fullScreenQuad_.vertexFormat_, lightPipelineLayout_, lightPipelineDesc, &directionalLightGIPipeline_);



  }

  void initialize(render::context_t& context)
  {
    context_ = &context;
    uvec2 size = uvec2(context.swapChain_.imageWidth_, context.swapChain_.imageHeight_);

    //Create allocator for uniform buffers and meshes
    render::gpuAllocatorCreate(context, 100 * 1024 * 1024, 0xFFFF, render::gpu_memory_type_e::HOST_VISIBLE_COHERENT, &allocator_);

    //Create descriptor pool
    render::descriptorPoolCreate(context, 1000u, 1000u, 1000u, 0u, 0u, &descriptorPool_);

    //Create vertex format (position + normal)
    uint32_t vertexSize = 2 * sizeof(maths::vec3) + sizeof(maths::vec2);
    render::vertex_attribute_t attributes[3] = { { render::vertex_attribute_t::format::VEC3, 0, vertexSize },
    { render::vertex_attribute_t::format::VEC3, sizeof(maths::vec3), vertexSize },
    { render::vertex_attribute_t::format::VEC2, 2 * sizeof(maths::vec3), vertexSize } };
    render::vertexFormatCreate(attributes, 3u, &vertexFormat_);

    //Load full-screen quad and sphere meshes
    fullScreenQuad_ = sample_utils::fullScreenQuad(context);
    mesh::createFromFile(context, "../resources/sphere.obj", mesh::EXPORT_POSITION_ONLY, nullptr, 0u, &sphereMesh_);

    //Create default diffuse map
    image::image2D_t defaultImage = {};
    defaultImage.width_ = defaultImage.height_ = 1u;
    defaultImage.componentCount_ = 4u;
    defaultImage.dataSize_ = 4;
    defaultImage.data_ = new uint8_t[4];
    defaultImage.data_[0] = 128u;
    defaultImage.data_[1] = defaultImage.data_[2] = defaultImage.data_[3] = 0u;
    render::texture2DCreate(context, &defaultImage, 1u, bkk::render::texture_sampler_t(), &defaultDiffuseMap_);
    delete[] defaultImage.data_;


    //Create globals uniform buffer
    camera_.position_ = vec3(-1.1f, 0.6f, -0.1f);
    camera_.angle_ = vec2(0.2f, 1.57f);
    camera_.Update();
    uniforms_.projectionMatrix_ = computePerspectiveProjectionMatrix(1.2f, (f32)size.x / (f32)size.y, 0.01f, 10.0f);
    computeInverse(uniforms_.projectionMatrix_, uniforms_.projectionInverseMatrix_);
    uniforms_.worldToViewMatrix_ = camera_.view_;
    uniforms_.viewToWorldMatrix_ = camera_.tx_;
    uniforms_.imageSize_ = vec4((f32)size.x, (f32)size.y, 1.0f / (f32)size.x, 1.0f / (f32)size.y);
    render::gpuBufferCreate(context, render::gpu_buffer_t::usage::UNIFORM_BUFFER, (void*)&uniforms_, sizeof(scene_uniforms_t), &allocator_, &globalsUbo_);



    //Create global descriptor set (Scene uniforms)   
    render::descriptor_binding_t binding = { render::descriptor_t::type::UNIFORM_BUFFER, 0, render::descriptor_t::stage::VERTEX | render::descriptor_t::stage::FRAGMENT };
    render::descriptorSetLayoutCreate(context, 1u, &binding, &globalsDescriptorSetLayout_);
    render::descriptor_t descriptor = render::getDescriptor(globalsUbo_);
    render::descriptorSetCreate(context, descriptorPool_, globalsDescriptorSetLayout_, &descriptor, &globalsDescriptorSet_);

    //Initialize off-screen render pass
    initializeOffscreenPass(context, size);

    //Presentation descriptor set layout and pipeline layout
    binding = { bkk::render::descriptor_t::type::COMBINED_IMAGE_SAMPLER, 0, bkk::render::descriptor_t::stage::FRAGMENT };
    bkk::render::descriptorSetLayoutCreate(context, 1u, &binding, &presentationDescriptorSetLayout_);
    bkk::render::pipelineLayoutCreate(context, 1u, &presentationDescriptorSetLayout_, &presentationPipelineLayout_);

    //Presentation descriptor sets
    descriptor = bkk::render::getDescriptor(finalImage_);
    bkk::render::descriptorSetCreate(context, descriptorPool_, presentationDescriptorSetLayout_, &descriptor, &presentationDescriptorSet_[0]);
    descriptor = bkk::render::getDescriptor(gBufferRT0_);
    bkk::render::descriptorSetCreate(context, descriptorPool_, presentationDescriptorSetLayout_, &descriptor, &presentationDescriptorSet_[1]);
    descriptor = bkk::render::getDescriptor(gBufferRT1_);
    bkk::render::descriptorSetCreate(context, descriptorPool_, presentationDescriptorSetLayout_, &descriptor, &presentationDescriptorSet_[2]);
    descriptor = bkk::render::getDescriptor(gBufferRT2_);
    bkk::render::descriptorSetCreate(context, descriptorPool_, presentationDescriptorSetLayout_, &descriptor, &presentationDescriptorSet_[3]);
    descriptor = bkk::render::getDescriptor(shadowMapRT0_);
    bkk::render::descriptorSetCreate(context, descriptorPool_, presentationDescriptorSetLayout_, &descriptor, &presentationDescriptorSet_[4]);
    descriptor = bkk::render::getDescriptor(shadowMapRT1_);
    bkk::render::descriptorSetCreate(context, descriptorPool_, presentationDescriptorSetLayout_, &descriptor, &presentationDescriptorSet_[5]);
    descriptor = bkk::render::getDescriptor(shadowMapRT2_);
    bkk::render::descriptorSetCreate(context, descriptorPool_, presentationDescriptorSetLayout_, &descriptor, &presentationDescriptorSet_[6]);

    //Create presentation pipeline
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::VERTEX_SHADER, gPresentationVertexShaderSource, &presentationVertexShader_);
    bkk::render::shaderCreateFromGLSLSource(context, bkk::render::shader_t::FRAGMENT_SHADER, gPresentationFragmentShaderSource, &presentationFragmentShader_);
    render::graphics_pipeline_t::description_t pipelineDesc = {};
    pipelineDesc.viewPort_ = { 0.0f, 0.0f, (float)context.swapChain_.imageWidth_, (float)context.swapChain_.imageHeight_, 0.0f, 1.0f };
    pipelineDesc.scissorRect_ = { { 0,0 },{ context.swapChain_.imageWidth_,context.swapChain_.imageHeight_ } };
    pipelineDesc.blendState_.resize(1);
    pipelineDesc.blendState_[0].colorWriteMask = 0xF;
    pipelineDesc.blendState_[0].blendEnable = VK_FALSE;
    pipelineDesc.cullMode_ = VK_CULL_MODE_BACK_BIT;
    pipelineDesc.depthTestEnabled_ = false;
    pipelineDesc.depthWriteEnabled_ = false;
    pipelineDesc.vertexShader_ = presentationVertexShader_;
    pipelineDesc.fragmentShader_ = presentationFragmentShader_;
    bkk::render::graphicsPipelineCreate(context, context.swapChain_.renderPass_, 0u, fullScreenQuad_.vertexFormat_, presentationPipelineLayout_, pipelineDesc, &presentationPipeline_);

    BuildPresentationCommandBuffers();
  }

  void Resize(uint32_t width, uint32_t height)
  {
    uniforms_.projectionMatrix_ = computePerspectiveProjectionMatrix(1.2f, (f32)width / (f32)height, 0.1f, 100.0f);
    uniforms_.imageSize_ = vec4((f32)width, (f32)height, 1.0f / (f32)width, 1.0f / (f32)height);
    render::swapchainResize(context_, width, height);
    BuildPresentationCommandBuffers();
  }

  void Render()
  {
    //Update scene
    transformManager_.update();

    //Update camera matrices
    uniforms_.worldToViewMatrix_ = camera_.view_;
    uniforms_.viewToWorldMatrix_ = camera_.tx_;
    render::gpuBufferUpdate(*context_, (void*)&uniforms_, 0u, sizeof(scene_uniforms_t), &globalsUbo_);

    //Update modelview matrices
    std::vector<object_t>& object(object_.getData());
    for (u32 i(0); i < object.size(); ++i)
    {
      render::gpuBufferUpdate(*context_, transformManager_.getWorldMatrix(object[i].transform_), 0, sizeof(mat4), &object[i].ubo_);
    }

    //Update lights position
    std::vector<point_light_t>& light(pointLight_.getData());
    for (u32 i(0); i<light.size(); ++i)
    {
      render::gpuBufferUpdate(*context_, &light[i].uniforms_.position_, 0, sizeof(vec4), &light[i].ubo_);
    }

    BuildAndSubmitCommandBuffer();
    render::presentNextImage(context_, 1u, &renderComplete_);
  }

  void BuildAndSubmitCommandBuffer()
  {
    //Render shadow map if there is a direcrtional light
    if (directionalLight_ != nullptr)
    {
      if (shadowCommandBuffer_.handle_ == VK_NULL_HANDLE)
      {
        render::commandBufferCreate(*context_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 0, nullptr, nullptr, 1, &shadowPassComplete_, render::command_buffer_t::GRAPHICS, &shadowCommandBuffer_);
      }

      if (generateDeferredCommandBuffer_)
      {
        VkClearValue clearValues[4];
        clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
        clearValues[1].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
        clearValues[2].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
        clearValues[3].depthStencil = { 1.0f,0 };

        render::commandBufferBegin(*context_, &shadowFrameBuffer_, 4u, clearValues, shadowCommandBuffer_);
        {

          //Shadow pass
          bkk::render::graphicsPipelineBind(shadowCommandBuffer_.handle_, shadowPipeline_);
          bkk::render::descriptorSetBindForGraphics(shadowCommandBuffer_.handle_, shadowPipelineLayout_, 0, &shadowGlobalsDescriptorSet_, 1u);
          packed_freelist_iterator_t<object_t> objectIter = object_.begin();
          while (objectIter != object_.end())
          {
            bkk::render::descriptorSetBindForGraphics(shadowCommandBuffer_.handle_, shadowPipelineLayout_, 1, &objectIter.get().descriptorSet_, 1u);
            bkk::render::descriptorSetBindForGraphics(shadowCommandBuffer_.handle_, shadowPipelineLayout_, 2, &material_.get(objectIter.get().material_)->descriptorSet_, 1u);
            mesh::mesh_t* mesh = mesh_.get(objectIter.get().mesh_);
            mesh::draw(shadowCommandBuffer_.handle_, *mesh);
            ++objectIter;
          }
        }
        render::commandBufferEnd(shadowCommandBuffer_);
        generateDeferredCommandBuffer_ = false;
      }

      render::commandBufferSubmit(*context_, shadowCommandBuffer_);
    }

    if (commandBuffer_.handle_ == VK_NULL_HANDLE)
    {
      if (directionalLight_ != nullptr)
      {
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        render::commandBufferCreate(*context_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1, &shadowPassComplete_, &waitStage, 1, &renderComplete_, render::command_buffer_t::GRAPHICS, &commandBuffer_);
      }
      else
      {
        render::commandBufferCreate(*context_, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 0, nullptr, nullptr, 1, &renderComplete_, render::command_buffer_t::GRAPHICS, &commandBuffer_);
      }
    }
    VkClearValue clearValues[5];
    clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clearValues[1].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clearValues[2].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clearValues[3].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
    clearValues[4].depthStencil = { 1.0f,0 };

    render::commandBufferBegin(*context_, &frameBuffer_, 5u, clearValues, commandBuffer_);
    {

      //GBuffer pass
      bkk::render::graphicsPipelineBind(commandBuffer_.handle_, gBufferPipeline_);
      bkk::render::descriptorSetBindForGraphics(commandBuffer_.handle_, gBufferPipelineLayout_, 0, &globalsDescriptorSet_, 1u);
      packed_freelist_iterator_t<object_t> objectIter = object_.begin();
      while (objectIter != object_.end())
      {
        bkk::render::descriptorSetBindForGraphics(commandBuffer_.handle_, gBufferPipelineLayout_, 1, &objectIter.get().descriptorSet_, 1u);
        bkk::render::descriptorSetBindForGraphics(commandBuffer_.handle_, gBufferPipelineLayout_, 2, &material_.get(objectIter.get().material_)->descriptorSet_, 1u);
        mesh::mesh_t* mesh = mesh_.get(objectIter.get().mesh_);
        mesh::draw(commandBuffer_.handle_, *mesh);
        ++objectIter;
      }

      //Light pass
      bkk::render::commandBufferNextSubpass(commandBuffer_);
      bkk::render::descriptorSetBindForGraphics(commandBuffer_.handle_, lightPipelineLayout_, 0, &globalsDescriptorSet_, 1u);
      bkk::render::descriptorSetBindForGraphics(commandBuffer_.handle_, lightPipelineLayout_, 1, &lightPassTexturesDescriptorSet_, 1u);

      //Point lights
      bkk::render::graphicsPipelineBind(commandBuffer_.handle_, pointLightPipeline_);
      packed_freelist_iterator_t<point_light_t> lightIter = pointLight_.begin();
      while (lightIter != pointLight_.end())
      {
        bkk::render::descriptorSetBindForGraphics(commandBuffer_.handle_, lightPipelineLayout_, 2, &lightIter.get().descriptorSet_, 1u);
        mesh::draw(commandBuffer_.handle_, sphereMesh_);
        ++lightIter;
      }

      //Directional light
      if (directionalLight_ != nullptr)
      {
        //bkk::render::graphicsPipelineBind(commandBuffer_.handle_, directionalLightPipeline_);
        if (globalIllumination_)
        {
          bkk::render::graphicsPipelineBind(commandBuffer_.handle_, directionalLightGIPipeline_);
        }
        else
        {
          bkk::render::graphicsPipelineBind(commandBuffer_.handle_, directionalLightPipeline_);
        }
        bkk::render::descriptorSetBindForGraphics(commandBuffer_.handle_, lightPipelineLayout_, 2, &directionalLight_->descriptorSet_, 1u);
        mesh::draw(commandBuffer_.handle_, fullScreenQuad_);
      }
    }
    render::commandBufferEnd(commandBuffer_);



    render::commandBufferSubmit(*context_, commandBuffer_);
  }

  void BuildPresentationCommandBuffers()
  {
    //Presentation command buffers
    for (unsigned i(0); i<3; ++i)
    {
      VkCommandBuffer cmdBuffer = bkk::render::beginPresentationCommandBuffer(*context_, i, nullptr);
      bkk::render::graphicsPipelineBind(cmdBuffer, presentationPipeline_);
      bkk::render::descriptorSetBindForGraphics(cmdBuffer, presentationPipelineLayout_, 0u, &presentationDescriptorSet_[currentPresentationDescriptorSet_], 1u);
      bkk::mesh::draw(cmdBuffer, fullScreenQuad_);
      bkk::render::endPresentationCommandBuffer(*context_, i);
    }
  }

  void OnKeyEvent(window::key_e key, bool pressed, scene_t& scene)
  {
    if (pressed)
    {
      switch (key)
      {
      case window::key_e::KEY_UP:
      case 'w':
      {
        camera_.Move(0.0f, -0.03f);
        break;
      }
      case window::key_e::KEY_DOWN:
      case 's':
      {
        camera_.Move(0.0f, 0.03f);
        break;
      }
      case window::key_e::KEY_LEFT:
      case 'a':
      {
        camera_.Move(-0.03f, 0.0f);
        break;
      }
      case window::key_e::KEY_RIGHT:
      case 'd':
      {
        camera_.Move(0.03f, 0.0f);
        break;
      }
      case window::key_e::KEY_1:
      case window::key_e::KEY_2:
      case window::key_e::KEY_3:
      case window::key_e::KEY_4:
      case window::key_e::KEY_5:
      case window::key_e::KEY_6:
      case window::key_e::KEY_7:
      {
        currentPresentationDescriptorSet_ = key - window::key_e::KEY_1;
        render::contextFlush(*context_);
        BuildPresentationCommandBuffers();
        break;
      }
      case window::key_e::KEY_G:
      {
        globalIllumination_ = !globalIllumination_;
        generateDeferredCommandBuffer_ = true;
        break;
      }
      default:
        break;
      }
    }
  }

  void OnMouseButton(bool pressed, uint32_t x, uint32_t y)
  {
    mouseButtonPressed_ = pressed;
    mousePosition_.x = (f32)x;
    mousePosition_.y = (f32)y;
  }

  void OnMouseMove(uint32_t x, uint32_t y)
  {
    if (mouseButtonPressed_)
    {
      f32 angleY = ((f32)x - mousePosition_.x) * 0.01f;
      f32 angleX = ((f32)y - mousePosition_.y) * 0.01f;
      mousePosition_.x = (f32)x;
      mousePosition_.y = (f32)y;
      camera_.Rotate(angleX, angleY);
    }
  }

  void Destroy()
  {
    //Clean-up
    render::contextFlush(*context_);

    //Destroy meshes
    packed_freelist_iterator_t<mesh::mesh_t> meshIter = mesh_.begin();
    while (meshIter != mesh_.end())
    {
      mesh::destroy(*context_, &meshIter.get(), &allocator_);
      ++meshIter;
    }

    //Destroy material resources
    packed_freelist_iterator_t<material_t> materialIter = material_.begin();
    while (materialIter != material_.end())
    {
      render::gpuBufferDestroy(*context_, &materialIter.get().ubo_, &allocator_);
      if (&materialIter.get().diffuseMap_.image_ != VK_NULL_HANDLE)
      {
        render::textureDestroy(*context_, &materialIter.get().diffuseMap_);
      }
      render::descriptorSetDestroy(*context_, &materialIter.get().descriptorSet_);
      ++materialIter;
    }

    //Destroy object resources
    packed_freelist_iterator_t<object_t> objectIter = object_.begin();
    while (objectIter != object_.end())
    {
      render::gpuBufferDestroy(*context_, &objectIter.get().ubo_, &allocator_);
      render::descriptorSetDestroy(*context_, &objectIter.get().descriptorSet_);
      ++objectIter;
    }

    //Destroy lights resources
    packed_freelist_iterator_t<point_light_t> lightIter = pointLight_.begin();
    while (lightIter != pointLight_.end())
    {
      render::gpuBufferDestroy(*context_, &lightIter.get().ubo_, &allocator_);
      render::descriptorSetDestroy(*context_, &lightIter.get().descriptorSet_);
      ++lightIter;
    }

    if (directionalLight_ != nullptr)
    {
      render::gpuBufferDestroy(*context_, &directionalLight_->ubo_, &allocator_);
      render::descriptorSetDestroy(*context_, &directionalLight_->descriptorSet_);
      delete directionalLight_;
    }

    render::shaderDestroy(*context_, &gBuffervertexShader_);
    render::shaderDestroy(*context_, &gBufferfragmentShader_);
    render::shaderDestroy(*context_, &pointLightVertexShader_);
    render::shaderDestroy(*context_, &pointLightFragmentShader_);
    render::shaderDestroy(*context_, &directionalLightVertexShader_);
    render::shaderDestroy(*context_, &directionalLightFragmentShader_);
    render::shaderDestroy(*context_, &directionalLightGIFragmentShader_);
    render::shaderDestroy(*context_, &shadowVertexShader_);
    render::shaderDestroy(*context_, &shadowFragmentShader_);
    render::shaderDestroy(*context_, &presentationVertexShader_);
    render::shaderDestroy(*context_, &presentationFragmentShader_);

    render::graphicsPipelineDestroy(*context_, &gBufferPipeline_);
    render::graphicsPipelineDestroy(*context_, &pointLightPipeline_);
    render::graphicsPipelineDestroy(*context_, &directionalLightPipeline_);
    render::graphicsPipelineDestroy(*context_, &directionalLightGIPipeline_);
    render::graphicsPipelineDestroy(*context_, &presentationPipeline_);
    render::graphicsPipelineDestroy(*context_, &shadowPipeline_);

    render::pipelineLayoutDestroy(*context_, &presentationPipelineLayout_);
    render::pipelineLayoutDestroy(*context_, &gBufferPipelineLayout_);
    render::pipelineLayoutDestroy(*context_, &lightPipelineLayout_);
    render::pipelineLayoutDestroy(*context_, &shadowPipelineLayout_);

    render::descriptorSetDestroy(*context_, &globalsDescriptorSet_);
    render::descriptorSetDestroy(*context_, &lightPassTexturesDescriptorSet_);
    render::descriptorSetDestroy(*context_, &presentationDescriptorSet_[0]);
    render::descriptorSetDestroy(*context_, &presentationDescriptorSet_[1]);
    render::descriptorSetDestroy(*context_, &presentationDescriptorSet_[2]);
    render::descriptorSetDestroy(*context_, &presentationDescriptorSet_[3]);
    render::descriptorSetDestroy(*context_, &presentationDescriptorSet_[4]);
    render::descriptorSetDestroy(*context_, &shadowGlobalsDescriptorSet_);

    render::descriptorSetLayoutDestroy(*context_, &globalsDescriptorSetLayout_);
    render::descriptorSetLayoutDestroy(*context_, &materialDescriptorSetLayout_);
    render::descriptorSetLayoutDestroy(*context_, &objectDescriptorSetLayout_);
    render::descriptorSetLayoutDestroy(*context_, &lightDescriptorSetLayout_);
    render::descriptorSetLayoutDestroy(*context_, &lightPassTexturesDescriptorSetLayout_);
    render::descriptorSetLayoutDestroy(*context_, &presentationDescriptorSetLayout_);
    render::descriptorSetLayoutDestroy(*context_, &shadowGlobalsDescriptorSetLayout_);


    render::textureDestroy(*context_, &gBufferRT0_);
    render::textureDestroy(*context_, &gBufferRT1_);
    render::textureDestroy(*context_, &gBufferRT2_);
    render::textureDestroy(*context_, &finalImage_);
    render::textureDestroy(*context_, &defaultDiffuseMap_);
    render::depthStencilBufferDestroy(*context_, &depthStencilBuffer_);
    render::textureDestroy(*context_, &shadowMapRT0_);
    render::textureDestroy(*context_, &shadowMapRT1_);
    render::textureDestroy(*context_, &shadowMapRT2_);
    render::depthStencilBufferDestroy(*context_, &shadowPassDepthStencilBuffer);

    mesh::destroy(*context_, &fullScreenQuad_);
    mesh::destroy(*context_, &sphereMesh_);

    render::frameBufferDestroy(*context_, &frameBuffer_);
    render::frameBufferDestroy(*context_, &shadowFrameBuffer_);

    render::commandBufferDestroy(*context_, &commandBuffer_);
    render::commandBufferDestroy(*context_, &shadowCommandBuffer_);

    render::renderPassDestroy(*context_, &renderPass_);
    render::renderPassDestroy(*context_, &shadowRenderPass_);

    render::vertexFormatDestroy(&vertexFormat_);
    render::gpuBufferDestroy(*context_, &globalsUbo_, &allocator_);
    render::gpuAllocatorDestroy(*context_, &allocator_);
    render::descriptorPoolDestroy(*context_, &descriptorPool_);
    vkDestroySemaphore(context_->device_, renderComplete_, nullptr);
    vkDestroySemaphore(context_->device_, shadowPassComplete_, nullptr);
  }

private:

  render::context_t* context_;

  bkk::transform_manager_t transformManager_;
  render::gpu_memory_allocator_t allocator_;

  packed_freelist_t<object_t> object_;
  packed_freelist_t<material_t> material_;
  packed_freelist_t<mesh::mesh_t> mesh_;
  packed_freelist_t<point_light_t> pointLight_;

  render::descriptor_pool_t descriptorPool_;
  render::descriptor_set_layout_t globalsDescriptorSetLayout_;
  render::descriptor_set_layout_t materialDescriptorSetLayout_;
  render::descriptor_set_layout_t objectDescriptorSetLayout_;
  render::descriptor_set_layout_t lightDescriptorSetLayout_;
  render::descriptor_set_layout_t lightPassTexturesDescriptorSetLayout_;
  render::descriptor_set_layout_t presentationDescriptorSetLayout_;

  uint32_t currentPresentationDescriptorSet_ = 0u;
  render::descriptor_set_t presentationDescriptorSet_[7];
  render::descriptor_set_t globalsDescriptorSet_;
  render::descriptor_set_t lightPassTexturesDescriptorSet_;

  render::vertex_format_t vertexFormat_;

  render::pipeline_layout_t gBufferPipelineLayout_;
  render::graphics_pipeline_t gBufferPipeline_;
  render::pipeline_layout_t lightPipelineLayout_;
  render::graphics_pipeline_t pointLightPipeline_;
  render::graphics_pipeline_t directionalLightPipeline_;
  render::graphics_pipeline_t directionalLightGIPipeline_;

  render::pipeline_layout_t presentationPipelineLayout_;
  render::graphics_pipeline_t presentationPipeline_;

  VkSemaphore renderComplete_;
  render::command_buffer_t commandBuffer_;
  render::render_pass_t renderPass_;

  scene_uniforms_t uniforms_;
  render::gpu_buffer_t globalsUbo_;

  render::frame_buffer_t frameBuffer_;
  render::texture_t gBufferRT0_;  //Albedo + roughness
  render::texture_t gBufferRT1_;  //Normal + Depth
  render::texture_t gBufferRT2_;  //F0 + metallic
  render::texture_t finalImage_;
  render::depth_stencil_buffer_t depthStencilBuffer_;

  render::shader_t gBuffervertexShader_;
  render::shader_t gBufferfragmentShader_;
  render::shader_t pointLightVertexShader_;
  render::shader_t pointLightFragmentShader_;
  render::shader_t directionalLightVertexShader_;
  render::shader_t directionalLightFragmentShader_;
  render::shader_t directionalLightGIFragmentShader_;
  render::shader_t presentationVertexShader_;
  render::shader_t presentationFragmentShader_;

  //Shadow pass
  uint32_t shadowMapSize_ = 4096;
  VkSemaphore shadowPassComplete_;
  render::command_buffer_t shadowCommandBuffer_;
  render::render_pass_t shadowRenderPass_;
  render::frame_buffer_t shadowFrameBuffer_;
  render::texture_t shadowMapRT0_;  //Depth + World space normal
  render::texture_t shadowMapRT1_;  //World space position
  render::texture_t shadowMapRT2_;  //Radiance

  render::depth_stencil_buffer_t shadowPassDepthStencilBuffer;
  render::descriptor_set_layout_t shadowGlobalsDescriptorSetLayout_;
  render::pipeline_layout_t shadowPipelineLayout_;
  render::graphics_pipeline_t shadowPipeline_;
  render::shader_t shadowVertexShader_;
  render::shader_t shadowFragmentShader_;
  render::descriptor_set_t shadowGlobalsDescriptorSet_;
  maths::mat4 worldToLightClipSpace_;

  render::texture_t defaultDiffuseMap_;
  mesh::mesh_t sphereMesh_;
  mesh::mesh_t fullScreenQuad_;

  directional_light_t* directionalLight_ = nullptr;
  sample_utils::free_camera_t camera_;
  maths::vec2 mousePosition_ = vec2(0.0f, 0.0f);
  bool mouseButtonPressed_ = false;

  bool generateDeferredCommandBuffer_ = true;
  bool globalIllumination_ = true;
};

int main()
{
  //Create a window
  window::window_t window;
  window::create("Global Illumination", 1200u, 800u, &window);

  //Initialize context
  render::context_t context;
  render::contextCreate("Global Illumination", "", window, 3, &context);

  //Initialize scene
  scene_t scene;
  scene.initialize(context);
  scene.load("../resources/sponza/sponza.obj");

  //Lights
  scene.addDirectionalLight(vec3(0.0, 1.5, 0.0), vec3(0.0f, 1.0f, 0.5f), vec3(5.0f, 5.0f, 5.0f), 0.1f);

  bool quit = false;
  while (!quit)
  {
    window::event_t* event = nullptr;
    while ((event = window::getNextEvent(&window)))
    {
      switch (event->type_)
      {
      case window::EVENT_QUIT:
      {
        quit = true;
        break;
      }
      case window::EVENT_RESIZE:
      {
        window::event_resize_t* resizeEvent = (window::event_resize_t*)event;
        scene.Resize(resizeEvent->width_, resizeEvent->height_);
        break;
      }
      case window::EVENT_KEY:
      {
        window::event_key_t* keyEvent = (window::event_key_t*)event;
        scene.OnKeyEvent(keyEvent->keyCode_, keyEvent->pressed_, scene);
        break;
      }
      case window::EVENT_MOUSE_BUTTON:
      {
        window::event_mouse_button_t* buttonEvent = (window::event_mouse_button_t*)event;
        scene.OnMouseButton(buttonEvent->pressed_, buttonEvent->x_, buttonEvent->y_);
        break;
      }
      case window::EVENT_MOUSE_MOVE:
      {
        window::event_mouse_move_t* moveEvent = (window::event_mouse_move_t*)event;
        scene.OnMouseMove(moveEvent->x_, moveEvent->y_);
        break;
      }
      default:
        break;
      }
    }

    //Render next frame
    scene.Render();
  }

  scene.Destroy();

  render::contextDestroy(&context);
  window::destroy(&window);
  return 0;
}