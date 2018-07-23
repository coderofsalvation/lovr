#include "graphics/graphics.h"
#include "graphics/canvas.h"
#include "graphics/mesh.h"
#include "graphics/shader.h"
#include "graphics/shaderBlock.h"
#include "graphics/texture.h"
#include "resources/shaders.h"
#include "data/modelData.h"
#include "math/mat4.h"
#include "lib/vec/vec.h"
#include <math.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if EMSCRIPTEN
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#else
#include "lib/glad/glad.h"
#endif

// Types

#define MAX_TEXTURES 16
#define MAX_UNIFORM_BUFFERS 32

#define LOVR_SHADER_POSITION 0
#define LOVR_SHADER_NORMAL 1
#define LOVR_SHADER_TEX_COORD 2
#define LOVR_SHADER_VERTEX_COLOR 3
#define LOVR_SHADER_TANGENT 4
#define LOVR_SHADER_BONES 5
#define LOVR_SHADER_BONE_WEIGHTS 6

static struct {
  Texture* defaultTexture;
  BlendMode blendMode;
  BlendAlphaMode blendAlphaMode;
  bool culling;
  bool depthEnabled;
  CompareMode depthTest;
  bool depthWrite;
  float lineWidth;
  bool stencilEnabled;
  CompareMode stencilMode;
  int stencilValue;
  bool stencilWriting;
  Winding winding;
  bool wireframe;
  Canvas* canvas[MAX_CANVASES];
  int canvasCount;
  uint32_t framebuffer;
  uint32_t indexBuffer;
  uint32_t program;
  Texture* textures[MAX_TEXTURES];
  uint32_t uniformBuffers[MAX_UNIFORM_BUFFERS];
  uint32_t vertexArray;
  uint32_t vertexBuffer;
  float viewport[4];
  bool srgb;
  bool supportsSinglepass;
  GraphicsLimits limits;
  GraphicsStats stats;
} state;

struct ShaderBlock {
  Ref ref;
  Uniform uniforms[32];
  int uniformCount;
  uint32_t buffer;
};

typedef struct {
  int index;
  int binding;
  ShaderBlock* source;
} UniformBlock;

typedef map_t(UniformBlock) map_block_t;

struct Shader {
  Ref ref;
  uint32_t program;
  map_uniform_t uniforms;
  map_block_t blocks;
  map_int_t attributes;
};

struct Texture {
  Ref ref;
  TextureType type;
  GLenum glType;
  TextureData** slices;
  int width;
  int height;
  int depth;
  int mipmapCount;
  GLuint id;
  TextureFilter filter;
  TextureWrap wrap;
  bool srgb;
  bool mipmaps;
  bool allocated;
};

struct Canvas {
  Texture texture;
  GLuint framebuffer;
  GLuint resolveFramebuffer;
  GLuint depthStencilBuffer;
  GLuint msaaTexture;
  CanvasFlags flags;
  Canvas** attachments[MAX_CANVASES];
};

typedef struct {
  Mesh* mesh;
  int attributeIndex;
  int divisor;
  bool enabled;
} MeshAttachment;

typedef map_t(MeshAttachment) map_attachment_t;

struct Mesh {
  Ref ref;
  uint32_t count;
  VertexFormat format;
  MeshDrawMode drawMode;
  GLenum usage;
  VertexPointer data;
  IndexPointer indices;
  uint32_t indexCount;
  size_t indexSize;
  size_t indexCapacity;
  bool mappedIndices;
  uint32_t dirtyStart;
  uint32_t dirtyEnd;
  uint32_t rangeStart;
  uint32_t rangeCount;
  GLuint vao;
  GLuint vbo;
  GLuint ibo;
  Material* material;
  float* pose;
  map_attachment_t attachments;
  MeshAttachment layout[MAX_ATTACHMENTS];
  bool isAttachment;
};

// Helper functions

static void gammaCorrectColor(Color* color) {
  if (state.srgb) {
    color->r = lovrMathGammaToLinear(color->r);
    color->g = lovrMathGammaToLinear(color->g);
    color->b = lovrMathGammaToLinear(color->b);
  }
}

static GLenum convertCompareMode(CompareMode mode) {
  switch (mode) {
    case COMPARE_NONE: return GL_ALWAYS;
    case COMPARE_EQUAL: return GL_EQUAL;
    case COMPARE_NEQUAL: return GL_NOTEQUAL;
    case COMPARE_LESS: return GL_LESS;
    case COMPARE_LEQUAL: return GL_LEQUAL;
    case COMPARE_GREATER: return GL_GREATER;
    case COMPARE_GEQUAL: return GL_GEQUAL;
  }
}

static GLenum convertWrapMode(WrapMode mode) {
  switch (mode) {
    case WRAP_CLAMP: return GL_CLAMP_TO_EDGE;
    case WRAP_REPEAT: return GL_REPEAT;
    case WRAP_MIRRORED_REPEAT: return GL_MIRRORED_REPEAT;
  }
}

static GLenum convertTextureFormat(TextureFormat format) {
  switch (format) {
    case FORMAT_RGB: return GL_RGB;
    case FORMAT_RGBA: return GL_RGBA;
    case FORMAT_RGBA4: return GL_RGBA;
    case FORMAT_RGBA16F: return GL_RGBA;
    case FORMAT_RGBA32F: return GL_RGBA;
    case FORMAT_R16F: return GL_RED;
    case FORMAT_R32F: return GL_RED;
    case FORMAT_RGB565: return GL_RGB;
    case FORMAT_RGB5A1: return GL_RGBA;
    case FORMAT_RGB10A2: return GL_RGBA;
    case FORMAT_RG11B10F: return GL_RGB;
    case FORMAT_DXT1: return GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
    case FORMAT_DXT3: return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
    case FORMAT_DXT5: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
  }
}

static GLenum convertTextureFormatInternal(TextureFormat format, bool srgb) {
  switch (format) {
    case FORMAT_RGB: return srgb ? GL_SRGB8 : GL_RGB8;
    case FORMAT_RGBA: return srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    case FORMAT_RGBA4: return GL_RGBA4;
    case FORMAT_RGBA16F: return GL_RGBA16F;
    case FORMAT_RGBA32F: return GL_RGBA32F;
    case FORMAT_R16F: return GL_R16F;
    case FORMAT_R32F: return GL_R32F;
    case FORMAT_RGB565: return GL_RGB565;
    case FORMAT_RGB5A1: return GL_RGB5_A1;
    case FORMAT_RGB10A2: return GL_RGB10_A2;
    case FORMAT_RG11B10F: return GL_R11F_G11F_B10F;
    case FORMAT_DXT1: return srgb ? GL_COMPRESSED_SRGB_S3TC_DXT1_EXT : GL_COMPRESSED_RGB_S3TC_DXT1_EXT;
    case FORMAT_DXT3: return srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT : GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
    case FORMAT_DXT5: return srgb ? GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
  }
}

static bool isTextureFormatCompressed(TextureFormat format) {
  switch (format) {
    case FORMAT_DXT1:
    case FORMAT_DXT3:
    case FORMAT_DXT5:
      return true;
    default:
      return false;
  }
}

static GLenum convertMeshUsage(MeshUsage usage) {
  switch (usage) {
    case MESH_STATIC: return GL_STATIC_DRAW;
    case MESH_DYNAMIC: return GL_DYNAMIC_DRAW;
    case MESH_STREAM: return GL_STREAM_DRAW;
  }
}

static GLenum convertMeshDrawMode(MeshDrawMode mode) {
  switch (mode) {
    case MESH_POINTS: return GL_POINTS;
    case MESH_LINES: return GL_LINES;
    case MESH_LINE_STRIP: return GL_LINE_STRIP;
    case MESH_LINE_LOOP: return GL_LINE_LOOP;
    case MESH_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case MESH_TRIANGLES: return GL_TRIANGLES;
    case MESH_TRIANGLE_FAN: return GL_TRIANGLE_FAN;
  }
}

static bool isCanvasFormatSupported(TextureFormat format) {
  switch (format) {
    case FORMAT_DXT1:
    case FORMAT_DXT3:
    case FORMAT_DXT5:
      return false;
    default: return true;
  }
}

static UniformType getUniformType(GLenum type, const char* debug) {
  switch (type) {
    case GL_FLOAT:
    case GL_FLOAT_VEC2:
    case GL_FLOAT_VEC3:
    case GL_FLOAT_VEC4:
      return UNIFORM_FLOAT;
    case GL_INT:
    case GL_INT_VEC2:
    case GL_INT_VEC3:
    case GL_INT_VEC4:
      return UNIFORM_INT;
    case GL_FLOAT_MAT2:
    case GL_FLOAT_MAT3:
    case GL_FLOAT_MAT4:
      return UNIFORM_MATRIX;
    case GL_SAMPLER_2D:
    case GL_SAMPLER_3D:
    case GL_SAMPLER_CUBE:
    case GL_SAMPLER_2D_ARRAY:
      return UNIFORM_SAMPLER;
    default:
      lovrThrow("Unsupported uniform type '%s'", debug);
      return UNIFORM_FLOAT;
  }
}

static int getUniformComponents(GLenum type) {
  switch (type) {
    case GL_FLOAT:
    case GL_INT:
    case GL_SAMPLER_2D:
    case GL_SAMPLER_3D:
    case GL_SAMPLER_CUBE:
    case GL_SAMPLER_2D_ARRAY:
      return 1;
    case GL_FLOAT_VEC2:
    case GL_INT_VEC2:
    case GL_FLOAT_MAT2:
      return 2;
    case GL_FLOAT_VEC3:
    case GL_INT_VEC3:
    case GL_FLOAT_MAT3:
      return 3;
    case GL_FLOAT_VEC4:
    case GL_INT_VEC4:
    case GL_FLOAT_MAT4:
      return 4;
    default:
      return 1;
  }
}

// GPU

static void lovrGpuBindFramebuffer(uint32_t framebuffer) {
  if (state.framebuffer != framebuffer) {
    state.framebuffer = framebuffer;
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  }
}

static void lovrGpuBindIndexBuffer(uint32_t indexBuffer) {
  if (state.indexBuffer != indexBuffer) {
    state.indexBuffer = indexBuffer;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
  }
}

void lovrGpuBindTexture(Texture* texture, int slot) {
  lovrAssert(slot >= 0 && slot < MAX_TEXTURES, "Invalid texture slot %d", slot);

  if (!texture) {
    if (!state.defaultTexture) {
      TextureData* textureData = lovrTextureDataGetBlank(1, 1, 0xff, FORMAT_RGBA);
      state.defaultTexture = lovrTextureCreate(TEXTURE_2D, &textureData, 1, true, false);
      lovrRelease(textureData);
    }

    texture = state.defaultTexture;
  }

  if (texture != state.textures[slot]) {
    lovrRetain(texture);
    lovrRelease(state.textures[slot]);
    state.textures[slot] = texture;
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(texture->glType, lovrTextureGetId(texture));
  }
}

void lovrGpuDirtyTexture(int slot) {
  lovrAssert(slot >= 0 && slot < MAX_TEXTURES, "Invalid texture slot %d", slot);
  state.textures[slot] = NULL;
}

static void lovrGpuBindUniformBuffer(uint32_t uniformBuffer, int slot) {
  if (state.uniformBuffers[slot] != uniformBuffer) {
    state.uniformBuffers[slot] = uniformBuffer;
    glBindBufferBase(GL_UNIFORM_BUFFER, slot, uniformBuffer);
  }
}

static void lovrGpuBindVertexArray(uint32_t vertexArray) {
  if (state.vertexArray != vertexArray) {
    state.vertexArray = vertexArray;
    glBindVertexArray(vertexArray);
  }
}

static void lovrGpuBindVertexBuffer(uint32_t vertexBuffer) {
  if (state.vertexBuffer != vertexBuffer) {
    state.vertexBuffer = vertexBuffer;
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
  }
}

static void lovrGpuSetViewport(float viewport[4]) {
  if (memcmp(state.viewport, viewport, 4 * sizeof(float))) {
    memcpy(state.viewport, viewport, 4 * sizeof(float));
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  }
}

static void lovrGpuUseProgram(uint32_t program) {
  if (state.program != program) {
    state.program = program;
    glUseProgram(program);
    state.stats.shaderSwitches++;
  }
}

void lovrGpuInit(bool srgb, gpuProc (*getProcAddress)(const char*)) {
#ifndef EMSCRIPTEN
  gladLoadGLLoader((GLADloadproc) getProcAddress);
  glEnable(GL_LINE_SMOOTH);
  glEnable(GL_PROGRAM_POINT_SIZE);
  if (srgb) {
    glEnable(GL_FRAMEBUFFER_SRGB);
  } else {
    glDisable(GL_FRAMEBUFFER_SRGB);
  }
  state.supportsSinglepass = GLAD_GL_ARB_viewport_array && GLAD_GL_NV_viewport_array2 && GLAD_GL_NV_stereo_view_rendering;
#endif
  glEnable(GL_BLEND);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  state.srgb = srgb;
  state.blendMode = -1;
  state.blendAlphaMode = -1;
  state.culling = false;
  state.depthEnabled = false;
  state.depthTest = COMPARE_LESS;
  state.depthWrite = true;
  state.lineWidth = 1;
  state.stencilEnabled = false;
  state.stencilMode = COMPARE_NONE;
  state.stencilValue = 0;
  state.stencilWriting = false;
  state.winding = WINDING_COUNTERCLOCKWISE;
  state.wireframe = false;
}

void lovrGpuDestroy() {
  lovrRelease(state.defaultTexture);
  for (int i = 0; i < MAX_TEXTURES; i++) {
    lovrRelease(state.textures[i]);
  }
}

void lovrGpuClear(Canvas** canvas, int canvasCount, Color* color, float* depth, int* stencil) {
  lovrGpuBindFramebuffer(canvasCount > 0 ? canvas[0]->framebuffer : 0);

  if (color) {
    gammaCorrectColor(color);
    float c[4] = { color->r, color->g, color->b, color->a };
    glClearBufferfv(GL_COLOR, 0, c);
    for (int i = 1; i < canvasCount; i++) {
      glClearBufferfv(GL_COLOR, i, c);
    }
  }

  if (depth) {
    if (!state.depthWrite) {
      state.depthWrite = true;
      glDepthMask(state.depthWrite);
    }

    glClearBufferfv(GL_DEPTH, 0, depth);
  }

  if (stencil) {
    glClearBufferiv(GL_STENCIL, 0, stencil);
  }

  if (canvasCount > 0) {
    lovrCanvasResolve(canvas[0]);
  }
}

void lovrGraphicsStencil(StencilAction action, int replaceValue, StencilCallback callback, void* userdata) {
  state.depthWrite = false;
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

  if (!state.stencilEnabled) {
    state.stencilEnabled = true;
    glEnable(GL_STENCIL_TEST);
  }

  GLenum glAction;
  switch (action) {
    case STENCIL_REPLACE: glAction = GL_REPLACE; break;
    case STENCIL_INCREMENT: glAction = GL_INCR; break;
    case STENCIL_DECREMENT: glAction = GL_DECR; break;
    case STENCIL_INCREMENT_WRAP: glAction = GL_INCR_WRAP; break;
    case STENCIL_DECREMENT_WRAP: glAction = GL_DECR_WRAP; break;
    case STENCIL_INVERT: glAction = GL_INVERT; break;
  }

  glStencilFunc(GL_ALWAYS, replaceValue, 0xff);
  glStencilOp(GL_KEEP, GL_KEEP, glAction);

  state.stencilWriting = true;
  callback(userdata);
  state.stencilWriting = false;

  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  state.stencilMode = ~0; // Dirty
}

void lovrGpuDraw(DrawCommand* command) {
  Mesh* mesh = command->mesh;
  Material* material = command->material;
  Shader* shader = command->shader;
  Pipeline* pipeline = &command->pipeline;
  int instances = command->instances;

  // Bind shader
  lovrGpuUseProgram(shader->program);

  // Blend mode
  if (state.blendMode != pipeline->blendMode || state.blendAlphaMode != pipeline->blendAlphaMode) {
    state.blendMode = pipeline->blendMode;
    state.blendAlphaMode = pipeline->blendAlphaMode;

    GLenum srcRGB = state.blendMode == BLEND_MULTIPLY ? GL_DST_COLOR : GL_ONE;
    if (srcRGB == GL_ONE && state.blendAlphaMode == BLEND_ALPHA_MULTIPLY) {
      srcRGB = GL_SRC_ALPHA;
    }

    switch (state.blendMode) {
      case BLEND_ALPHA:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(srcRGB, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;

      case BLEND_ADD:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(srcRGB, GL_ONE, GL_ZERO, GL_ONE);
        break;

      case BLEND_SUBTRACT:
        glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
        glBlendFuncSeparate(srcRGB, GL_ONE, GL_ZERO, GL_ONE);
        break;

      case BLEND_MULTIPLY:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(srcRGB, GL_ZERO, GL_DST_COLOR, GL_ZERO);
        break;

      case BLEND_LIGHTEN:
        glBlendEquation(GL_MAX);
        glBlendFuncSeparate(srcRGB, GL_ZERO, GL_ONE, GL_ZERO);
        break;

      case BLEND_DARKEN:
        glBlendEquation(GL_MIN);
        glBlendFuncSeparate(srcRGB, GL_ZERO, GL_ONE, GL_ZERO);
        break;

      case BLEND_SCREEN:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(srcRGB, GL_ONE_MINUS_SRC_COLOR, GL_ONE, GL_ONE_MINUS_SRC_COLOR);
        break;

      case BLEND_REPLACE:
        glBlendEquation(GL_FUNC_ADD);
        glBlendFuncSeparate(srcRGB, GL_ZERO, GL_ONE, GL_ZERO);
        break;
    }
  }

  // Culling
  if (state.culling != pipeline->culling) {
    state.culling = pipeline->culling;
    if (state.culling) {
      glEnable(GL_CULL_FACE);
    } else {
      glDisable(GL_CULL_FACE);
    }
  }

  // Depth test
  if (state.depthTest != pipeline->depthTest) {
    state.depthTest = pipeline->depthTest;
    if (state.depthTest != COMPARE_NONE) {
      if (!state.depthEnabled) {
        state.depthEnabled = true;
        glEnable(GL_DEPTH_TEST);
      }
      glDepthFunc(convertCompareMode(state.depthTest));
    } else if (state.depthEnabled) {
      state.depthEnabled = false;
      glDisable(GL_DEPTH_TEST);
    }
  }

  // Depth write
  if (state.depthWrite != pipeline->depthWrite) {
    state.depthWrite = pipeline->depthWrite;
    glDepthMask(state.depthWrite);
  }

  // Line width
  if (state.lineWidth != pipeline->lineWidth) {
    state.lineWidth = pipeline->lineWidth;
    glLineWidth(state.lineWidth);
  }

  // Stencil mode
  if (!state.stencilWriting && (state.stencilMode != pipeline->stencilMode || state.stencilValue != pipeline->stencilValue)) {
    state.stencilMode = pipeline->stencilMode;
    state.stencilValue = pipeline->stencilValue;
    if (state.stencilMode != COMPARE_NONE) {
      if (!state.stencilEnabled) {
        state.stencilEnabled = true;
        glEnable(GL_STENCIL_TEST);
      }

      GLenum glMode = GL_ALWAYS;
      switch (state.stencilMode) {
        case COMPARE_EQUAL: glMode = GL_EQUAL; break;
        case COMPARE_NEQUAL: glMode = GL_NOTEQUAL; break;
        case COMPARE_LESS: glMode = GL_GREATER; break;
        case COMPARE_LEQUAL: glMode = GL_GEQUAL; break;
        case COMPARE_GREATER: glMode = GL_LESS; break;
        case COMPARE_GEQUAL: glMode = GL_LEQUAL; break;
        default: break;
      }

      glStencilFunc(glMode, state.stencilValue, 0xff);
      glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    } else if (state.stencilEnabled) {
      state.stencilEnabled = false;
      glDisable(GL_STENCIL_TEST);
    }
  }

  // Winding
  if (state.winding != pipeline->winding) {
    state.winding = pipeline->winding;
    glFrontFace(state.winding == WINDING_CLOCKWISE ? GL_CW : GL_CCW);
  }

  // Wireframe
  if (state.wireframe != pipeline->wireframe) {
    state.wireframe = pipeline->wireframe;
#ifndef EMSCRIPTEN
    glPolygonMode(GL_FRONT_AND_BACK, state.wireframe ? GL_LINE : GL_FILL);
#endif
  }

  // Transform
  lovrShaderSetMatrix(shader, "lovrModel", command->transform, 16);
  lovrShaderSetMatrix(shader, "lovrViews", command->camera.viewMatrix[0], 32);
  lovrShaderSetMatrix(shader, "lovrProjections", command->camera.projection[0], 32);

  float modelView[32];
  mat4_multiply(mat4_set(modelView, command->camera.viewMatrix[0]), command->transform);
  mat4_multiply(mat4_set(modelView + 16, command->camera.viewMatrix[1]), command->transform);
  lovrShaderSetMatrix(shader, "lovrTransforms", modelView, 32);

  if (lovrShaderHasUniform(shader, "lovrNormalMatrices")) {
    if (mat4_invert(modelView) && mat4_invert(modelView + 16)) {
      mat4_transpose(modelView);
      mat4_transpose(modelView + 16);
    } else {
      mat4_identity(modelView);
      mat4_identity(modelView + 16);
    }

    float normalMatrices[18] = {
      modelView[0], modelView[1], modelView[2],
      modelView[4], modelView[5], modelView[6],
      modelView[8], modelView[9], modelView[10],

      modelView[16], modelView[17], modelView[18],
      modelView[20], modelView[21], modelView[22],
      modelView[24], modelView[25], modelView[26]
    };

    lovrShaderSetMatrix(shader, "lovrNormalMatrices", normalMatrices, 18);
  }

  // Pose
  float* pose = lovrMeshGetPose(mesh);
  if (pose) {
    lovrShaderSetMatrix(shader, "lovrPose", pose, MAX_BONES * 16);
  } else {
    float identity[16];
    mat4_identity(identity);
    lovrShaderSetMatrix(shader, "lovrPose", identity, 16);
  }

  // Point size
  lovrShaderSetFloat(shader, "lovrPointSize", &pipeline->pointSize, 1);

  // Color
  Color color = pipeline->color;
  gammaCorrectColor(&color);
  float data[4] = { color.r, color.g, color.b, color.a };
  lovrShaderSetFloat(shader, "lovrColor", data, 4);

  // Material
  for (int i = 0; i < MAX_MATERIAL_SCALARS; i++) {
    float value = lovrMaterialGetScalar(material, i);
    lovrShaderSetFloat(shader, lovrShaderScalarUniforms[i], &value, 1);
  }

  for (int i = 0; i < MAX_MATERIAL_COLORS; i++) {
    Color color = lovrMaterialGetColor(material, i);
    gammaCorrectColor(&color);
    float data[4] = { color.r, color.g, color.b, color.a };
    lovrShaderSetFloat(shader, lovrShaderColorUniforms[i], data, 4);
  }

  for (int i = 0; i < MAX_MATERIAL_TEXTURES; i++) {
    Texture* texture = lovrMaterialGetTexture(material, i);
    lovrShaderSetTexture(shader, lovrShaderTextureUniforms[i], &texture, 1);
  }

  lovrShaderSetMatrix(shader, "lovrMaterialTransform", material->transform, 9);

  // Canvas
  Canvas** canvas = pipeline->canvasCount > 0 ? pipeline->canvas : &command->camera.canvas;
  int canvasCount = pipeline->canvasCount > 0 ? pipeline->canvasCount : (command->camera.canvas != NULL);
  if (canvasCount != state.canvasCount || memcmp(state.canvas, canvas, canvasCount * sizeof(Canvas*))) {
    if (state.canvasCount > 0) {
      lovrCanvasResolve(state.canvas[0]);
    }

    state.canvasCount = canvasCount;

    if (canvasCount > 0) {
      memcpy(state.canvas, canvas, canvasCount * sizeof(Canvas*));
      lovrGpuBindFramebuffer(canvas[0]->framebuffer);

      GLenum buffers[MAX_CANVASES];
      for (int i = 0; i < canvasCount; i++) {
        buffers[i] = GL_COLOR_ATTACHMENT0 + i;
        glFramebufferTexture2D(GL_FRAMEBUFFER, buffers[i], GL_TEXTURE_2D, lovrTextureGetId((Texture*) canvas[i]), 0);
      }
      glDrawBuffers(canvasCount, buffers);

      GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      lovrAssert(status != GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS, "All multicanvas canvases must have the same dimensions");
      lovrAssert(status == GL_FRAMEBUFFER_COMPLETE, "Unable to bind framebuffer");
    } else {
      lovrGpuBindFramebuffer(0);
    }
  }

  // Bind attributes
  lovrMeshBind(mesh, shader);

  bool stereo = pipeline->canvasCount == 0 && command->camera.stereo == true;
  int drawCount = 1 + (stereo == true && !state.supportsSinglepass);

  // Draw (TODEW)
  for (int i = 0; i < drawCount; i++) {
    if (pipeline->canvasCount > 0) {
      int width = lovrTextureGetWidth((Texture*) pipeline->canvas[0], 0);
      int height = lovrTextureGetHeight((Texture*) pipeline->canvas[0], 0);
      lovrGpuSetViewport((float[4]) { 0, 0, width, height });
#ifndef EMSCRIPTEN
    } else if (state.supportsSinglepass) {
      glViewportArrayv(0, 2, command->camera.viewport[0]);
#endif
    } else  {
      lovrGpuSetViewport(command->camera.viewport[i]);
    }

    // Bind uniforms
    int eye = (stereo && state.supportsSinglepass) ? -1 : i;
    lovrShaderSetInt(shader, "lovrEye", &eye, 1);
    lovrShaderBind(shader);

    uint32_t rangeStart, rangeCount;
    lovrMeshGetDrawRange(mesh, &rangeStart, &rangeCount);
    uint32_t indexCount;
    size_t indexSize;
    lovrMeshReadIndices(mesh, &indexCount, &indexSize);
    GLenum glDrawMode = convertMeshDrawMode(lovrMeshGetDrawMode(mesh));
    if (indexCount > 0) {
      size_t count = rangeCount ? rangeCount : indexCount;
      GLenum indexType = indexSize == sizeof(uint16_t) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
      size_t offset = rangeStart * indexSize;
      if (instances > 1) {
        glDrawElementsInstanced(glDrawMode, count, indexType, (GLvoid*) offset, instances);
      } else {
        glDrawElements(glDrawMode, count, indexType, (GLvoid*) offset);
      }
    } else {
      size_t count = rangeCount ? rangeCount : lovrMeshGetVertexCount(mesh);
      if (instances > 1) {
        glDrawArraysInstanced(glDrawMode, rangeStart, count, instances);
      } else {
        glDrawArrays(glDrawMode, rangeStart, count);
      }
    }

    state.stats.drawCalls++;
  }
}

void lovrGpuPresent() {
  memset(&state.stats, 0, sizeof(state.stats));
}

GraphicsLimits lovrGraphicsGetLimits() {
  if (!state.limits.initialized) {
#ifdef EMSCRIPTEN
    glGetFloatv(GL_ALIASED_POINT_SIZE_RANGE, state.limits.pointSizes);
#else
    glGetFloatv(GL_POINT_SIZE_RANGE, state.limits.pointSizes);
#endif
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &state.limits.textureSize);
    glGetIntegerv(GL_MAX_SAMPLES, &state.limits.textureMSAA);
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &state.limits.textureAnisotropy);
    state.limits.initialized = 1;
  }

  return state.limits;
}

GraphicsStats lovrGraphicsGetStats() {
  return state.stats;
}

// Texture

static void lovrTextureAllocate(Texture* texture, TextureData* textureData) {
  int w = textureData->width;
  int h = textureData->height;
  int d = texture->depth;
  texture->allocated = true;
  texture->width = w;
  texture->height = h;

  if (texture->mipmaps) {
    int dimension = texture->type == TEXTURE_VOLUME ? (MAX(MAX(w, h), d)) : MAX(w, h);
    texture->mipmapCount = texture->mipmaps ? (log2(dimension) + 1) : 1;
  } else {
    texture->mipmapCount = 1;
  }

  if (isTextureFormatCompressed(textureData->format)) {
    return;
  }

  bool srgb = lovrGraphicsIsGammaCorrect() && texture->srgb;
  GLenum glFormat = convertTextureFormat(textureData->format);
  GLenum internalFormat = convertTextureFormatInternal(textureData->format, srgb);
#ifndef EMSCRIPTEN
  if (GLAD_GL_ARB_texture_storage) {
#endif
  if (texture->type == TEXTURE_ARRAY) {
    glTexStorage3D(texture->glType, texture->mipmapCount, internalFormat, w, h, d);
  } else {
    glTexStorage2D(texture->glType, texture->mipmapCount, internalFormat, w, h);
  }
#ifndef EMSCRIPTEN
  } else {
    for (int i = 0; i < texture->mipmapCount; i++) {
      switch (texture->type) {
        case TEXTURE_2D:
          glTexImage2D(texture->glType, i, internalFormat, w, h, 0, glFormat, GL_UNSIGNED_BYTE, NULL);
          break;

        case TEXTURE_CUBE:
          for (int face = 0; face < 6; face++) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, i, internalFormat, w, h, 0, glFormat, GL_UNSIGNED_BYTE, NULL);
          }
          break;

        case TEXTURE_ARRAY:
        case TEXTURE_VOLUME:
          glTexImage3D(texture->glType, i, internalFormat, w, h, d, 0, glFormat, GL_UNSIGNED_BYTE, NULL);
          break;
      }
      w = MAX(w >> 1, 1);
      h = MAX(h >> 1, 1);
      d = texture->type == TEXTURE_VOLUME ? MAX(d >> 1, 1) : d;
    }
  }
#endif
}

Texture* lovrTextureCreate(TextureType type, TextureData** slices, int depth, bool srgb, bool mipmaps) {
  Texture* texture = lovrAlloc(Texture, lovrTextureDestroy);
  if (!texture) return NULL;

  texture->type = type;
  switch (type) {
    case TEXTURE_2D: texture->glType = GL_TEXTURE_2D; break;
    case TEXTURE_ARRAY: texture->glType = GL_TEXTURE_2D_ARRAY; break;
    case TEXTURE_CUBE: texture->glType = GL_TEXTURE_CUBE_MAP; break;
    case TEXTURE_VOLUME: texture->glType = GL_TEXTURE_3D; break;
  }

  texture->slices = calloc(depth, sizeof(TextureData**));
  texture->depth = depth;
  texture->srgb = srgb;
  texture->mipmaps = mipmaps;

  WrapMode wrap = type == TEXTURE_CUBE ? WRAP_CLAMP : WRAP_REPEAT;
  glGenTextures(1, &texture->id);
  lovrGpuBindTexture(texture, 0);
  lovrTextureSetFilter(texture, lovrGraphicsGetDefaultFilter());
  lovrTextureSetWrap(texture, (TextureWrap) { .s = wrap, .t = wrap, .r = wrap });

  lovrAssert(type != TEXTURE_CUBE || depth == 6, "6 images are required for a cube texture\n");
  lovrAssert(type != TEXTURE_2D || depth == 1, "2D textures can only contain a single image");

  if (slices) {
    for (int i = 0; i < depth; i++) {
      lovrTextureReplacePixels(texture, slices[i], 0, 0, i, 0);
    }
  }

  return texture;
}

void lovrTextureDestroy(void* ref) {
  Texture* texture = ref;
  for (int i = 0; i < texture->depth; i++) {
    lovrRelease(texture->slices[i]);
  }
  glDeleteTextures(1, &texture->id);
  free(texture->slices);
  free(texture);
}

GLuint lovrTextureGetId(Texture* texture) {
  return texture->id;
}

int lovrTextureGetWidth(Texture* texture, int mipmap) {
  return MAX(texture->width >> mipmap, 1);
}

int lovrTextureGetHeight(Texture* texture, int mipmap) {
  return MAX(texture->height >> mipmap, 1);
}

int lovrTextureGetDepth(Texture* texture, int mipmap) {
  return texture->type == TEXTURE_VOLUME ? MAX(texture->depth >> mipmap, 1) : texture->depth;
}

int lovrTextureGetMipmapCount(Texture* texture) {
  return texture->mipmapCount;
}

TextureType lovrTextureGetType(Texture* texture) {
  return texture->type;
}

void lovrTextureReplacePixels(Texture* texture, TextureData* textureData, int x, int y, int slice, int mipmap) {
  lovrRetain(textureData);
  lovrRelease(texture->slices[slice]);
  texture->slices[slice] = textureData;
  lovrGpuBindTexture(texture, 0);

  if (!texture->allocated) {
    lovrAssert(texture->type != TEXTURE_CUBE || textureData->width == textureData->height, "Cubemap images must be square");
    lovrTextureAllocate(texture, textureData);
  }

  if (!textureData->blob.data) {
    return;
  }

  int width = lovrTextureGetWidth(texture, mipmap);
  int height = lovrTextureGetHeight(texture, mipmap);
  bool overflow = (x + textureData->width > width) || (y + textureData->height > height);
  lovrAssert(!overflow, "Trying to replace pixels outside the texture's bounds");
  lovrAssert(mipmap >= 0 && mipmap < texture->mipmapCount, "Invalid mipmap level %d", mipmap);
  GLenum glFormat = convertTextureFormat(textureData->format);
  GLenum glInternalFormat = convertTextureFormatInternal(textureData->format, texture->srgb);
  GLenum binding = (texture->type == TEXTURE_CUBE) ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice : texture->glType;

  if (isTextureFormatCompressed(textureData->format)) {
    lovrAssert(width == textureData->width && height == textureData->height, "Compressed texture pixels must be fully replaced");
    lovrAssert(mipmap == 0, "Unable to replace a specific mipmap of a compressed texture");
    Mipmap m; int i;
    vec_foreach(&textureData->mipmaps, m, i) {
      switch (texture->type) {
        case TEXTURE_2D:
        case TEXTURE_CUBE:
          glCompressedTexImage2D(binding, i, glInternalFormat, m.width, m.height, 0, m.size, m.data);
          break;
        case TEXTURE_ARRAY:
        case TEXTURE_VOLUME:
          glCompressedTexSubImage3D(binding, i, x, y, slice, m.width, m.height, 1, glInternalFormat, m.size, m.data);
          break;
      }
    }
  } else {
    switch (texture->type) {
      case TEXTURE_2D:
      case TEXTURE_CUBE:
        glTexSubImage2D(binding, mipmap, x, y, textureData->width, textureData->height, glFormat, GL_UNSIGNED_BYTE, textureData->blob.data);
        break;
      case TEXTURE_ARRAY:
      case TEXTURE_VOLUME:
        glTexSubImage3D(binding, mipmap, x, y, slice, textureData->width, textureData->height, 1, glFormat, GL_UNSIGNED_BYTE, textureData->blob.data);
        break;
    }

    if (texture->mipmaps) {
      glGenerateMipmap(texture->glType);
    }
  }
}

TextureFilter lovrTextureGetFilter(Texture* texture) {
  return texture->filter;
}

void lovrTextureSetFilter(Texture* texture, TextureFilter filter) {
  float anisotropy = filter.mode == FILTER_ANISOTROPIC ? MAX(filter.anisotropy, 1.) : 1.;
  lovrGpuBindTexture(texture, 0);
  texture->filter = filter;

  switch (filter.mode) {
    case FILTER_NEAREST:
      glTexParameteri(texture->glType, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(texture->glType, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      break;

    case FILTER_BILINEAR:
      if (texture->mipmaps) {
        glTexParameteri(texture->glType, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
        glTexParameteri(texture->glType, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      } else {
        glTexParameteri(texture->glType, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(texture->glType, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      }
      break;

    case FILTER_TRILINEAR:
    case FILTER_ANISOTROPIC:
      if (texture->mipmaps) {
        glTexParameteri(texture->glType, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(texture->glType, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      } else {
        glTexParameteri(texture->glType, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(texture->glType, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      }
      break;
  }

  glTexParameteri(texture->glType, GL_TEXTURE_MAX_ANISOTROPY_EXT, anisotropy);
}

TextureWrap lovrTextureGetWrap(Texture* texture) {
  return texture->wrap;
}

void lovrTextureSetWrap(Texture* texture, TextureWrap wrap) {
  texture->wrap = wrap;
  lovrGpuBindTexture(texture, 0);
  glTexParameteri(texture->glType, GL_TEXTURE_WRAP_S, convertWrapMode(wrap.s));
  glTexParameteri(texture->glType, GL_TEXTURE_WRAP_T, convertWrapMode(wrap.t));
  if (texture->type == TEXTURE_CUBE || texture->type == TEXTURE_VOLUME) {
    glTexParameteri(texture->glType, GL_TEXTURE_WRAP_R, convertWrapMode(wrap.r));
  }
}

// Canvas

Canvas* lovrCanvasCreate(int width, int height, TextureFormat format, CanvasFlags flags) {
  lovrAssert(isCanvasFormatSupported(format), "Unsupported texture format for Canvas");

  TextureData* textureData = lovrTextureDataGetEmpty(width, height, format);
  Texture* texture = lovrTextureCreate(TEXTURE_2D, &textureData, 1, true, flags.mipmaps);
  if (!texture) return NULL;

  Canvas* canvas = lovrAlloc(Canvas, lovrCanvasDestroy);
  Ref ref = canvas->texture.ref;
  canvas->texture = *texture;
  canvas->texture.ref = ref;
  canvas->flags = flags;

  // Framebuffer
  glGenFramebuffers(1, &canvas->framebuffer);
  lovrGpuBindFramebuffer(canvas->framebuffer);

  // Color attachment
  if (flags.msaa > 0) {
    GLenum internalFormat = convertTextureFormatInternal(format, lovrGraphicsIsGammaCorrect());
    glGenRenderbuffers(1, &canvas->msaaTexture);
    glBindRenderbuffer(GL_RENDERBUFFER, canvas->msaaTexture);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, flags.msaa, internalFormat, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, canvas->msaaTexture);
  } else {
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, canvas->texture.id, 0);
  }

  // Depth/Stencil
  if (flags.depth || flags.stencil) {
    GLenum depthStencilFormat = flags.stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24;
    glGenRenderbuffers(1, &canvas->depthStencilBuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, canvas->depthStencilBuffer);
    if (flags.msaa > 0) {
      glRenderbufferStorageMultisample(GL_RENDERBUFFER, flags.msaa, depthStencilFormat, width, height);
    } else {
      glRenderbufferStorage(GL_RENDERBUFFER, depthStencilFormat, width, height);
    }

    if (flags.depth) {
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, canvas->depthStencilBuffer);
    }

    if (flags.stencil) {
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, canvas->depthStencilBuffer);
    }
  }

  // Resolve framebuffer
  if (flags.msaa > 0) {
    glGenFramebuffers(1, &canvas->resolveFramebuffer);
    lovrGpuBindFramebuffer(canvas->resolveFramebuffer);
    glBindTexture(GL_TEXTURE_2D, canvas->texture.id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, canvas->texture.id, 0);
    lovrGpuBindFramebuffer(canvas->framebuffer);
  }

  lovrAssert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Error creating Canvas");
  lovrGpuClear(&canvas, 1, &(Color) { 0, 0, 0, 0 }, &(float) { 1. }, &(int) { 0 });

  return canvas;
}

void lovrCanvasDestroy(void* ref) {
  Canvas* canvas = ref;
  glDeleteFramebuffers(1, &canvas->framebuffer);
  if (canvas->resolveFramebuffer) {
    glDeleteFramebuffers(1, &canvas->resolveFramebuffer);
  }
  if (canvas->depthStencilBuffer) {
    glDeleteRenderbuffers(1, &canvas->depthStencilBuffer);
  }
  if (canvas->msaaTexture) {
    glDeleteTextures(1, &canvas->msaaTexture);
  }
  lovrTextureDestroy(ref);
}

void lovrCanvasResolve(Canvas* canvas) {
  if (canvas->flags.msaa > 0) {
    int width = canvas->texture.width;
    int height = canvas->texture.height;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, canvas->framebuffer);
    lovrGpuBindFramebuffer(canvas->resolveFramebuffer);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  }

  if (canvas->flags.mipmaps) {
    lovrGpuBindTexture(&canvas->texture, 0);
    glGenerateMipmap(canvas->texture.glType);
  }
}

TextureFormat lovrCanvasGetFormat(Canvas* canvas) {
  return canvas->texture.slices[0]->format;
}

int lovrCanvasGetMSAA(Canvas* canvas) {
  return canvas->flags.msaa;
}

TextureData* lovrCanvasNewTextureData(Canvas* canvas) {
  TextureData* textureData = lovrTextureDataGetBlank(canvas->texture.width, canvas->texture.height, 0, FORMAT_RGBA);
  if (!textureData) return NULL;

  lovrGpuBindFramebuffer(canvas->framebuffer);
  glReadPixels(0, 0, canvas->texture.width, canvas->texture.height, GL_RGBA, GL_UNSIGNED_BYTE, textureData->blob.data);

  return textureData;
}

// Shader

static GLuint compileShader(GLenum type, const char** sources, int count) {
  GLuint shader = glCreateShader(type);

  glShaderSource(shader, count, sources, NULL);
  glCompileShader(shader);

  int isShaderCompiled;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &isShaderCompiled);
  if (!isShaderCompiled) {
    int logLength;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);

    char* log = malloc(logLength);
    glGetShaderInfoLog(shader, logLength, &logLength, log);
    lovrThrow("Could not compile shader %s", log);
  }

  return shader;
}

static GLuint linkShaders(GLuint vertexShader, GLuint fragmentShader) {
  GLuint program = glCreateProgram();
  glAttachShader(program, vertexShader);
  glAttachShader(program, fragmentShader);
  glBindAttribLocation(program, LOVR_SHADER_POSITION, "lovrPosition");
  glBindAttribLocation(program, LOVR_SHADER_NORMAL, "lovrNormal");
  glBindAttribLocation(program, LOVR_SHADER_TEX_COORD, "lovrTexCoord");
  glBindAttribLocation(program, LOVR_SHADER_VERTEX_COLOR, "lovrVertexColor");
  glBindAttribLocation(program, LOVR_SHADER_TANGENT, "lovrTangent");
  glBindAttribLocation(program, LOVR_SHADER_BONES, "lovrBones");
  glBindAttribLocation(program, LOVR_SHADER_BONE_WEIGHTS, "lovrBoneWeights");
  glLinkProgram(program);

  int isLinked;
  glGetProgramiv(program, GL_LINK_STATUS, &isLinked);
  if (!isLinked) {
    int logLength;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);

    char* log = malloc(logLength);
    glGetProgramInfoLog(program, logLength, &logLength, log);
    lovrThrow("Could not link shader %s", log);
  }

  glDetachShader(program, vertexShader);
  glDeleteShader(vertexShader);
  glDetachShader(program, fragmentShader);
  glDeleteShader(fragmentShader);

  return program;
}

Shader* lovrShaderCreate(const char* vertexSource, const char* fragmentSource) {
  Shader* shader = lovrAlloc(Shader, lovrShaderDestroy);
  if (!shader) return NULL;

  // Vertex
  vertexSource = vertexSource == NULL ? lovrDefaultVertexShader : vertexSource;
  const char* vertexSources[] = { lovrShaderVertexPrefix, vertexSource, lovrShaderVertexSuffix };
  GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSources, 3);

  // Fragment
  fragmentSource = fragmentSource == NULL ? lovrDefaultFragmentShader : fragmentSource;
  const char* fragmentSources[] = { lovrShaderFragmentPrefix, fragmentSource, lovrShaderFragmentSuffix };
  GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSources, 3);

  // Link
  uint32_t program = linkShaders(vertexShader, fragmentShader);
  shader->program = program;

  lovrGpuUseProgram(program);
  glVertexAttrib4fv(LOVR_SHADER_VERTEX_COLOR, (float[4]) { 1., 1., 1., 1. });
  glVertexAttribI4iv(LOVR_SHADER_BONES, (int[4]) { 0., 0., 0., 0. });
  glVertexAttrib4fv(LOVR_SHADER_BONE_WEIGHTS, (float[4]) { 1., 0., 0., 0. });

  // Uniform introspection
  int32_t uniformCount;
  int textureSlot = 0;
  map_init(&shader->uniforms);
  glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformCount);
  for (int i = 0; i < uniformCount; i++) {
    Uniform uniform;
    GLenum glType;
    glGetActiveUniform(program, i, LOVR_MAX_UNIFORM_LENGTH, NULL, &uniform.count, &glType, uniform.name);

    char* subscript = strchr(uniform.name, '[');
    if (subscript) {
      *subscript = '\0';
    }

    uniform.location = glGetUniformLocation(program, uniform.name);
    uniform.type = getUniformType(glType, uniform.name);
    uniform.components = getUniformComponents(glType);
    uniform.baseTextureSlot = (uniform.type == UNIFORM_SAMPLER) ? textureSlot : -1;

    if (uniform.location == -1) {
      continue;
    }

    switch (uniform.type) {
      case UNIFORM_FLOAT:
        uniform.size = uniform.components * uniform.count * sizeof(float);
        uniform.value.data = calloc(1, uniform.size);
        break;

      case UNIFORM_INT:
        uniform.size = uniform.components * uniform.count * sizeof(int);
        uniform.value.data = calloc(1, uniform.size);
        break;

      case UNIFORM_MATRIX:
        uniform.size = uniform.components * uniform.components * uniform.count * sizeof(int);
        uniform.value.data = calloc(1, uniform.size);
        break;

      case UNIFORM_SAMPLER:
        uniform.size = uniform.components * uniform.count * MAX(sizeof(Texture*), sizeof(int));
        uniform.value.data = calloc(1, uniform.size);

        // Use the value for ints to bind texture slots, but use the value for textures afterwards.
        for (int i = 0; i < uniform.count; i++) {
          uniform.value.ints[i] = uniform.baseTextureSlot + i;
        }
        glUniform1iv(uniform.location, uniform.count, uniform.value.ints);
        break;
    }

    size_t offset = 0;
    for (int j = 0; j < uniform.count; j++) {
      int location = uniform.location;

      if (uniform.count > 1) {
        char name[LOVR_MAX_UNIFORM_LENGTH];
        snprintf(name, LOVR_MAX_UNIFORM_LENGTH, "%s[%d]", uniform.name, j);
        location = glGetUniformLocation(program, name);
      }

      switch (uniform.type) {
        case UNIFORM_FLOAT:
          glGetUniformfv(program, location, &uniform.value.floats[offset]);
          offset += uniform.components;
          break;

        case UNIFORM_INT:
          glGetUniformiv(program, location, &uniform.value.ints[offset]);
          offset += uniform.components;
          break;

        case UNIFORM_MATRIX:
          glGetUniformfv(program, location, &uniform.value.floats[offset]);
          offset += uniform.components * uniform.components;
          break;

        default:
          break;
      }
    }

    map_set(&shader->uniforms, uniform.name, uniform);
    textureSlot += (uniform.type == UNIFORM_SAMPLER) ? uniform.count : 0;
  }

  // Uniform blocks
  int32_t blockCount;
  glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &blockCount);
  map_init(&shader->blocks);
  for (int i = 0; i < blockCount; i++) {
    char name[LOVR_MAX_UNIFORM_LENGTH];
    glGetActiveUniformBlockName(program, i, LOVR_MAX_UNIFORM_LENGTH, NULL, name);
    UniformBlock block = { .index = i, .binding = i + 1, .source = NULL };
    glUniformBlockBinding(program, block.index, block.binding);
    map_set(&shader->blocks, name, block);
  }

  // Attribute cache
  int32_t attributeCount;
  glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &attributeCount);
  map_init(&shader->attributes);
  for (int i = 0; i < attributeCount; i++) {
    char name[LOVR_MAX_ATTRIBUTE_LENGTH];
    GLint size;
    GLenum type;
    glGetActiveAttrib(program, i, LOVR_MAX_ATTRIBUTE_LENGTH, NULL, &size, &type, name);
    map_set(&shader->attributes, name, glGetAttribLocation(program, name));
  }

  return shader;
}

Shader* lovrShaderCreateDefault(DefaultShader type) {
  switch (type) {
    case SHADER_DEFAULT: return lovrShaderCreate(NULL, NULL);
    case SHADER_CUBE: return lovrShaderCreate(lovrCubeVertexShader, lovrCubeFragmentShader); break;
    case SHADER_PANO: return lovrShaderCreate(lovrCubeVertexShader, lovrPanoFragmentShader); break;
    case SHADER_FONT: return lovrShaderCreate(NULL, lovrFontFragmentShader);
    case SHADER_FILL: return lovrShaderCreate(lovrFillVertexShader, NULL);
    default: lovrThrow("Unknown default shader type"); return NULL;
  }
}

void lovrShaderDestroy(void* ref) {
  Shader* shader = ref;
  glDeleteProgram(shader->program);
  const char* key;
  map_iter_t iter = map_iter(&shader->uniforms);
  while ((key = map_next(&shader->uniforms, &iter)) != NULL) {
    Uniform* uniform = map_get(&shader->uniforms, key);
    free(uniform->value.data);
  }
  map_deinit(&shader->uniforms);
  map_deinit(&shader->attributes);
  free(shader);
}

void lovrShaderBind(Shader* shader) {
  map_iter_t iter = map_iter(&shader->uniforms);
  const char* key;
  while ((key = map_next(&shader->uniforms, &iter)) != NULL) {
    Uniform* uniform = map_get(&shader->uniforms, key);

    if (uniform->type != UNIFORM_SAMPLER && !uniform->dirty) {
      continue;
    }

    uniform->dirty = false;
    int count = uniform->count;
    void* data = uniform->value.data;

    switch (uniform->type) {
      case UNIFORM_FLOAT:
        switch (uniform->components) {
          case 1: glUniform1fv(uniform->location, count, data); break;
          case 2: glUniform2fv(uniform->location, count, data); break;
          case 3: glUniform3fv(uniform->location, count, data); break;
          case 4: glUniform4fv(uniform->location, count, data); break;
        }
        break;

      case UNIFORM_INT:
        switch (uniform->components) {
          case 1: glUniform1iv(uniform->location, count, data); break;
          case 2: glUniform2iv(uniform->location, count, data); break;
          case 3: glUniform3iv(uniform->location, count, data); break;
          case 4: glUniform4iv(uniform->location, count, data); break;
        }
        break;

      case UNIFORM_MATRIX:
        switch (uniform->components) {
          case 2: glUniformMatrix2fv(uniform->location, count, GL_FALSE, data); break;
          case 3: glUniformMatrix3fv(uniform->location, count, GL_FALSE, data); break;
          case 4: glUniformMatrix4fv(uniform->location, count, GL_FALSE, data); break;
        }
        break;

      case UNIFORM_SAMPLER:
        for (int i = 0; i < count; i++) {
          lovrGpuBindTexture(uniform->value.textures[i], uniform->baseTextureSlot + i);
        }
        break;
    }
  }

  iter = map_iter(&shader->blocks);
  while ((key = map_next(&shader->blocks, &iter)) != NULL) {
    UniformBlock* block = map_get(&shader->blocks, key);
    lovrGpuBindUniformBuffer(block->source ? block->source->buffer : 0, block->binding);
  }
}

int lovrShaderGetAttributeId(Shader* shader, const char* name) {
  int* id = map_get(&shader->attributes, name);
  return id ? *id : -1;
}

bool lovrShaderHasUniform(Shader* shader, const char* name) {
  return map_get(&shader->uniforms, name) != NULL;
}

bool lovrShaderGetUniform(Shader* shader, const char* name, int* count, int* components, size_t* size, UniformType* type) {
  Uniform* uniform = map_get(&shader->uniforms, name);
  if (!uniform) {
    return false;
  }

  *count = uniform->count;
  *components = uniform->components;
  *size = uniform->size;
  *type = uniform->type;
  return true;
}

static void lovrShaderSetUniform(Shader* shader, const char* name, UniformType type, void* data, int count, size_t size, const char* debug) {
  Uniform* uniform = map_get(&shader->uniforms, name);
  if (!uniform) {
    return;
  }

  const char* plural = (uniform->size / size) > 1 ? "s" : "";
  lovrAssert(uniform->type == type, "Unable to send %ss to uniform %s", debug, uniform->name);
  lovrAssert(count * size <= uniform->size, "Expected at most %d %s%s for uniform %s, got %d", uniform->size / size, debug, plural, uniform->name, count);

  if (!uniform->dirty && !memcmp(uniform->value.data, data, count * size)) {
    return;
  }

  memcpy(uniform->value.data, data, count * size);
  uniform->dirty = true;
}

void lovrShaderSetFloat(Shader* shader, const char* name, float* data, int count) {
  lovrShaderSetUniform(shader, name, UNIFORM_FLOAT, data, count, sizeof(float), "float");
}

void lovrShaderSetInt(Shader* shader, const char* name, int* data, int count) {
  lovrShaderSetUniform(shader, name, UNIFORM_INT, data, count, sizeof(int), "int");
}

void lovrShaderSetMatrix(Shader* shader, const char* name, float* data, int count) {
  lovrShaderSetUniform(shader, name, UNIFORM_MATRIX, data, count, sizeof(float), "float");
}

void lovrShaderSetTexture(Shader* shader, const char* name, Texture** data, int count) {
  lovrShaderSetUniform(shader, name, UNIFORM_SAMPLER, data, count, sizeof(Texture*), "texture");
}

// ShaderBlock

ShaderBlock* lovrShaderBlockCreate(Uniform uniforms[MAX_SHADER_BLOCK_UNIFORMS], int uniformCount) {
  ShaderBlock* block = lovrAlloc(sizeof(ShaderBlock), lovrShaderBlockDestroy);
  if (!block) return NULL;

  memcpy(block->uniforms, uniforms, MAX_SHADER_BLOCK_UNIFORMS * sizeof(Uniform));
  block->uniformCount = uniformCount;

  size_t offset = 0;
  for (int i = 0; i < uniformCount; i++) {
    uniforms[i].offset = offset;
    if (uniforms[i].type != UNIFORM_MATRIX) {
      if (uniforms[i].count == 1) {
        offset += 4;
      } else {
        offset += uniforms[i].count * 16;
      }
    } else {
      offset += uniforms[i].components * uniforms[i].components * 4;
    }
  }

  size_t totalSize = offset;
  glGenBuffers(1, &block->buffer);
  lovrGpuBindUniformBuffer(block->buffer, 0);
  glBufferData(GL_UNIFORM_BUFFER, totalSize, NULL, GL_STATIC_DRAW);

  return block;
}

void lovrShaderBlockDestroy(void* ref) {
  free(ref);
}

// Mesh

Mesh* lovrMeshCreate(uint32_t count, VertexFormat format, MeshDrawMode drawMode, MeshUsage usage) {
  Mesh* mesh = lovrAlloc(Mesh, lovrMeshDestroy);
  if (!mesh) return NULL;

  mesh->count = count;
  mesh->format = format;
  mesh->drawMode = drawMode;
  mesh->usage = convertMeshUsage(usage);

  glGenBuffers(1, &mesh->vbo);
  glGenBuffers(1, &mesh->ibo);
  lovrGpuBindVertexBuffer(mesh->vbo);
  glBufferData(GL_ARRAY_BUFFER, count * format.stride, NULL, mesh->usage);
  glGenVertexArrays(1, &mesh->vao);

  map_init(&mesh->attachments);
  for (int i = 0; i < format.count; i++) {
    map_set(&mesh->attachments, format.attributes[i].name, ((MeshAttachment) { mesh, i, 0, true }));
  }

  mesh->data.raw = calloc(count, format.stride);

  return mesh;
}

void lovrMeshDestroy(void* ref) {
  Mesh* mesh = ref;
  lovrRelease(mesh->material);
  free(mesh->data.raw);
  free(mesh->indices.raw);
  glDeleteBuffers(1, &mesh->vbo);
  glDeleteBuffers(1, &mesh->ibo);
  glDeleteVertexArrays(1, &mesh->vao);
  const char* key;
  map_iter_t iter = map_iter(&mesh->attachments);
  while ((key = map_next(&mesh->attachments, &iter)) != NULL) {
    MeshAttachment* attachment = map_get(&mesh->attachments, key);
    if (attachment->mesh != mesh) {
      lovrRelease(attachment->mesh);
    }
  }
  map_deinit(&mesh->attachments);
  free(mesh);
}

void lovrMeshAttachAttribute(Mesh* mesh, Mesh* other, const char* name, int divisor) {
  MeshAttachment* otherAttachment = map_get(&other->attachments, name);
  lovrAssert(!mesh->isAttachment, "Attempted to attach to a mesh which is an attachment itself");
  lovrAssert(otherAttachment, "No attribute named '%s' exists", name);
  lovrAssert(!map_get(&mesh->attachments, name), "Mesh already has an attribute named '%s'", name);
  lovrAssert(divisor >= 0, "Divisor can't be negative");

  MeshAttachment attachment = { other, otherAttachment->attributeIndex, divisor, true };
  map_set(&mesh->attachments, name, attachment);
  other->isAttachment = true;
  lovrRetain(other);
}

void lovrMeshDetachAttribute(Mesh* mesh, const char* name) {
  MeshAttachment* attachment = map_get(&mesh->attachments, name);
  lovrAssert(attachment, "No attached attribute '%s' was found", name);
  lovrAssert(attachment->mesh != mesh, "Attribute '%s' was not attached from another Mesh", name);
  lovrRelease(attachment->mesh);
  map_remove(&mesh->attachments, name);
}

void lovrMeshBind(Mesh* mesh, Shader* shader) {
  const char* key;
  map_iter_t iter = map_iter(&mesh->attachments);

  MeshAttachment layout[MAX_ATTACHMENTS];
  memset(layout, 0, MAX_ATTACHMENTS * sizeof(MeshAttachment));

  lovrGpuBindVertexArray(mesh->vao);
  lovrMeshUnmapVertices(mesh);
  lovrMeshUnmapIndices(mesh);
  if (mesh->indexCount > 0) {
    lovrGpuBindIndexBuffer(mesh->ibo);
  }

  while ((key = map_next(&mesh->attachments, &iter)) != NULL) {
    int location = lovrShaderGetAttributeId(shader, key);

    if (location >= 0) {
      MeshAttachment* attachment = map_get(&mesh->attachments, key);
      layout[location] = *attachment;
      lovrMeshUnmapVertices(attachment->mesh);
      lovrMeshUnmapIndices(attachment->mesh);
    }
  }

  for (int i = 0; i < MAX_ATTACHMENTS; i++) {
    MeshAttachment previous = mesh->layout[i];
    MeshAttachment current = layout[i];

    if (!memcmp(&previous, &current, sizeof(MeshAttachment))) {
      continue;
    }

    if (previous.enabled != current.enabled) {
      if (current.enabled) {
        glEnableVertexAttribArray(i);
      } else {
        glDisableVertexAttribArray(i);
        mesh->layout[i] = current;
        continue;
      }
    }

    if (previous.divisor != current.divisor) {
      glVertexAttribDivisor(i, current.divisor);
    }

    if (previous.mesh != current.mesh || previous.attributeIndex != current.attributeIndex) {
      lovrGpuBindVertexBuffer(current.mesh->vbo);
      VertexFormat* format = &current.mesh->format;
      Attribute attribute = format->attributes[current.attributeIndex];
      switch (attribute.type) {
        case ATTR_FLOAT:
          glVertexAttribPointer(i, attribute.count, GL_FLOAT, GL_TRUE, format->stride, (void*) attribute.offset);
          break;

        case ATTR_BYTE:
          glVertexAttribPointer(i, attribute.count, GL_UNSIGNED_BYTE, GL_TRUE, format->stride, (void*) attribute.offset);
          break;

        case ATTR_INT:
          glVertexAttribIPointer(i, attribute.count, GL_UNSIGNED_INT, format->stride, (void*) attribute.offset);
          break;
      }
    }

    mesh->layout[i] = current;
  }
}

VertexFormat* lovrMeshGetVertexFormat(Mesh* mesh) {
  return &mesh->format;
}

MeshDrawMode lovrMeshGetDrawMode(Mesh* mesh) {
  return mesh->drawMode;
}

void lovrMeshSetDrawMode(Mesh* mesh, MeshDrawMode drawMode) {
  mesh->drawMode = drawMode;
}

int lovrMeshGetVertexCount(Mesh* mesh) {
  return mesh->count;
}

bool lovrMeshIsAttributeEnabled(Mesh* mesh, const char* name) {
  MeshAttachment* attachment = map_get(&mesh->attachments, name);
  lovrAssert(attachment, "Mesh does not have an attribute named '%s'", name);
  return attachment->enabled;
}

void lovrMeshSetAttributeEnabled(Mesh* mesh, const char* name, bool enable) {
  MeshAttachment* attachment = map_get(&mesh->attachments, name);
  lovrAssert(attachment, "Mesh does not have an attribute named '%s'", name);
  attachment->enabled = enable;
}

void lovrMeshGetDrawRange(Mesh* mesh, uint32_t* start, uint32_t* count) {
  *start = mesh->rangeStart;
  *count = mesh->rangeCount;
}

void lovrMeshSetDrawRange(Mesh* mesh, uint32_t start, uint32_t count) {
  uint32_t limit = mesh->indexCount > 0 ? mesh->indexCount : mesh->count;
  lovrAssert(start + count <= limit, "Invalid mesh draw range [%d, %d]", start + 1, start + count + 1);
  mesh->rangeStart = start;
  mesh->rangeCount = count;
}

Material* lovrMeshGetMaterial(Mesh* mesh) {
  return mesh->material;
}

void lovrMeshSetMaterial(Mesh* mesh, Material* material) {
  if (mesh->material != material) {
    lovrRetain(material);
    lovrRelease(mesh->material);
    mesh->material = material;
  }
}

float* lovrMeshGetPose(Mesh* mesh) {
  return mesh->pose;
}

void lovrMeshSetPose(Mesh* mesh, float* pose) {
  mesh->pose = pose;
}

VertexPointer lovrMeshMapVertices(Mesh* mesh, uint32_t start, uint32_t count, bool read, bool write) {
  if (write) {
    mesh->dirtyStart = MIN(mesh->dirtyStart, start);
    mesh->dirtyEnd = MAX(mesh->dirtyEnd, start + count);
  }

  return (VertexPointer) { .bytes = mesh->data.bytes + start * mesh->format.stride };
}

void lovrMeshUnmapVertices(Mesh* mesh) {
  if (mesh->dirtyEnd == 0) {
    return;
  }

  size_t stride = mesh->format.stride;
  lovrGpuBindVertexBuffer(mesh->vbo);
  if (mesh->usage == MESH_STREAM) {
    glBufferData(GL_ARRAY_BUFFER, mesh->count * stride, mesh->data.bytes, mesh->usage);
  } else {
    size_t offset = mesh->dirtyStart * stride;
    size_t count = (mesh->dirtyEnd - mesh->dirtyStart) * stride;
    glBufferSubData(GL_ARRAY_BUFFER, offset, count, mesh->data.bytes + offset);
  }

  mesh->dirtyStart = INT_MAX;
  mesh->dirtyEnd = 0;
}

IndexPointer lovrMeshReadIndices(Mesh* mesh, uint32_t* count, size_t* size) {
  *size = mesh->indexSize;
  *count = mesh->indexCount;

  if (mesh->indexCount == 0) {
    return (IndexPointer) { .raw = NULL };
  } else if (mesh->mappedIndices) {
    lovrMeshUnmapIndices(mesh);
  }

  return mesh->indices;
}

IndexPointer lovrMeshWriteIndices(Mesh* mesh, uint32_t count, size_t size) {
  if (mesh->mappedIndices) {
    lovrMeshUnmapIndices(mesh);
  }

  mesh->indexSize = size;
  mesh->indexCount = count;

  if (count == 0) {
    return (IndexPointer) { .raw = NULL };
  }

  lovrGpuBindVertexArray(mesh->vao);
  lovrGpuBindIndexBuffer(mesh->ibo);
  mesh->mappedIndices = true;

  if (mesh->indexCapacity < size * count) {
    mesh->indexCapacity = nextPo2(size * count);
    mesh->indices.raw = realloc(mesh->indices.raw, mesh->indexCapacity);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh->indexCapacity, NULL, mesh->usage);
  }

  return mesh->indices;
}

void lovrMeshUnmapIndices(Mesh* mesh) {
  if (!mesh->mappedIndices) {
    return;
  }

  mesh->mappedIndices = false;
  lovrGpuBindIndexBuffer(mesh->ibo);
  glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, mesh->indexCount * mesh->indexSize, mesh->indices.raw);
}

void lovrMeshResize(Mesh* mesh, uint32_t count) {
  if (mesh->count < count) {
    mesh->count = nextPo2(count);
    lovrGpuBindVertexBuffer(mesh->vbo);
    mesh->data.raw = realloc(mesh->data.raw, count * mesh->format.stride);
    glBufferData(GL_ARRAY_BUFFER, count * mesh->format.stride, mesh->data.raw, mesh->usage);
  }
}
