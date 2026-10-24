#pragma once

// Optional independent macOS oracle: compile the unchanged pinned GLSL shader.
// Production FIL-19B never includes this helper or creates a graphics context.
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
namespace photospider_oracle::smaa_gpu {
template <class Pixel>
struct GpuStages {
  std::vector<Pixel> edge, weight, output;
};
inline GLuint shader(GLenum stage, const std::string& source) {
  const auto handle = glCreateShader(stage);
  const char* text = source.c_str();
  glShaderSource(handle, 1, &text, nullptr);
  glCompileShader(handle);
  GLint ok = 0;
  glGetShaderiv(handle, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char message[32768];
    glGetShaderInfoLog(handle, sizeof message, nullptr, message);
    throw std::runtime_error(message);
  }
  return handle;
}
inline GLuint program(const std::string& fragment) {
  const auto vs = shader(GL_VERTEX_SHADER,
                         "#version 410 core\nvoid main(){vec2 "
                         "p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position="
                         "vec4(p*2.0-1.0,0,1);}\n");
  const auto fs = shader(GL_FRAGMENT_SHADER, fragment);
  const auto p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char message[32768];
    glGetProgramInfoLog(p, sizeof message, nullptr, message);
    throw std::runtime_error(message);
  }
  glDeleteShader(vs);
  glDeleteShader(fs);
  return p;
}
inline GLuint texture(int w, int h, GLenum internal, GLenum format, GLenum type,
                      const void* data) {
  GLuint t;
  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, data);
  return t;
}
template <class Texture>
auto upstream_gpu(const Texture& source, bool byte_weights,
                  const unsigned char* area_data,
                  const unsigned char* search_data,
                  const std::string& shader_path) {
  using Pixel = typename std::decay_t<decltype(source.pixels)>::value_type;
  static_assert(sizeof(Pixel) == 4 * sizeof(float));
  struct ContextOwner {
    CGLPixelFormatObj pixel = nullptr;
    CGLContextObj context = nullptr;
    CGLContextObj previous = CGLGetCurrentContext();
    ~ContextOwner() {
      if (context) {
        CGLSetCurrentContext(previous);
        CGLDestroyContext(context);
      }
      if (pixel)
        CGLDestroyPixelFormat(pixel);
    }
  } owner;
  CGLPixelFormatAttribute attrs[] = {
      kCGLPFAOpenGLProfile,
      static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),
      kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers,
      static_cast<CGLPixelFormatAttribute>(0)};
  auto& pixel = owner.pixel;
  auto& context = owner.context;
  GLint np;
  if (CGLChoosePixelFormat(attrs, &pixel, &np) ||
      CGLCreateContext(pixel, nullptr, &context))
    throw std::runtime_error("CGL context");
  CGLSetCurrentContext(context);
  const int w = source.width, h = source.height;
  std::ifstream file(shader_path, std::ios::binary);
  if (!file)
    throw std::runtime_error("pinned shader file unavailable");
  const std::string upstream((std::istreambuf_iterator<char>(file)), {});
  const std::string preamble =
      "#version 410 core\n"
      "#define SMAA_GLSL_3 1\n#define SMAA_PRESET_LOW 1\n"
      "#define SMAA_RT_METRICS metrics\nuniform vec4 metrics;\n";
  const std::string declarations =
      "\nuniform sampler2D "
      "colorTex,edgesTex,areaTex,searchTex,blendTex;\nlayout(location=0) out "
      "vec4 fragmentColor;\n";
  const std::string mains[] = {
      "void main(){vec2 uv=gl_FragCoord.xy*metrics.xy;vec4 "
      "offsets[3];SMAAEdgeDetectionVS(uv,offsets);fragmentColor=vec4("
      "SMAALumaEdgeDetectionPS(uv,offsets,colorTex),0,1);}",
      "void main(){vec2 uv=gl_FragCoord.xy*metrics.xy;vec2 pix;vec4 "
      "offsets[3];SMAABlendingWeightCalculationVS(uv,pix,offsets);"
      "fragmentColor=SMAABlendingWeightCalculationPS(uv,pix,offsets,edgesTex,"
      "areaTex,searchTex,vec4(0));}",
      "void main(){vec2 uv=gl_FragCoord.xy*metrics.xy;vec4 "
      "offset;SMAANeighborhoodBlendingVS(uv,offset);fragmentColor="
      "SMAANeighborhoodBlendingPS(uv,offset,colorTex,blendTex);}"};
  const GLuint color =
      texture(w, h, GL_RGBA32F, GL_RGBA, GL_FLOAT, source.pixels.data());
  const GLuint edges = texture(w, h, GL_RGBA32F, GL_RGBA, GL_FLOAT, nullptr);
  const GLuint weights = texture(w, h, byte_weights ? GL_RGBA8 : GL_RGBA32F,
                                 GL_RGBA, GL_FLOAT, nullptr);
  const GLuint result = texture(w, h, GL_RGBA32F, GL_RGBA, GL_FLOAT, nullptr);
  const GLuint area =
      texture(160, 560, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, area_data);
  const GLuint search =
      texture(64, 16, GL_R8, GL_RED, GL_UNSIGNED_BYTE, search_data);
  GLuint framebuffer, vao;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glViewport(0, 0, w, h);
  glDisable(GL_BLEND);
  glDisable(GL_DITHER);
  const GLuint targets[] = {edges, weights, result};
  GpuStages<Pixel> values;
  std::vector<Pixel>* outputs[] = {&values.edge, &values.weight,
                                   &values.output};
  const GLuint textures[] = {color, edges, area, search, weights};
  const char* names[] = {"colorTex", "edgesTex", "areaTex", "searchTex",
                         "blendTex"};
  for (unsigned stage = 0; stage < 3; ++stage) {
    const auto p = program(preamble + upstream + declarations + mains[stage]);
    glUseProgram(p);
    glUniform4f(glGetUniformLocation(p, "metrics"), 1.0F / w, 1.0F / h,
                static_cast<float>(w), static_cast<float>(h));
    for (unsigned unit = 0; unit < 5; ++unit) {
      glActiveTexture(GL_TEXTURE0 + unit);
      glBindTexture(GL_TEXTURE_2D, textures[unit]);
      glUniform1i(glGetUniformLocation(p, names[unit]), unit);
      if (unit == 0) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        stage == 0 ? GL_NEAREST : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                        stage == 0 ? GL_NEAREST : GL_LINEAR);
      }
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           targets[stage], 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      throw std::runtime_error("framebuffer");
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    outputs[stage]->resize(w * h);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, outputs[stage]->data());
    const auto error = glGetError();
    if (error)
      throw std::runtime_error("GL error " + std::to_string(error));
    glDeleteProgram(p);
  }
  glDeleteFramebuffers(1, &framebuffer);
  glDeleteVertexArrays(1, &vao);
  for (auto t : {color, edges, weights, result, area, search})
    glDeleteTextures(1, &t);
  return values;
}

}  // namespace photospider_oracle::smaa_gpu
