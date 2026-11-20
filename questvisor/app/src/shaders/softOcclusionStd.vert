#version 430
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// Video panel with soft depth occlusion (see softOcclusion.frag). Plain unlit pass-through: object -> world -> clip,
// raw UV straight to the fragment shader (the material carries no UV transforms).

#include <metaSpatialSdkVertexBase.glsl>

void main() {
  App2VertexUnpacked app = getApp2VertexUnpacked();

  vertexOut.color = vec4(1.0);
  vertexOut.albedoCoord = app.uv;
  vertexOut.roughnessMetallicCoord = app.uv;
  vertexOut.emissiveCoord = app.uv;
  vertexOut.occlusionCoord = app.uv;
  vertexOut.normalCoord = app.uv;
  vertexOut.lighting = vec3(1.0);

  vec4 wPos4 = g_PrimitiveUniform.worldFromObject * vec4(app.position, 1.0);
  vertexOut.worldPosition = wPos4.xyz;
  vertexOut.worldNormal = normalize((transpose(g_PrimitiveUniform.objectFromWorld) * vec4(app.normal, 0.0)).xyz);

  gl_Position = getClipFromWorld() * wPos4;
  postprocessPosition(gl_Position);
}
