// Soft depth occlusion for the video panel.
//
// The SDK's built-in occlusion is a hard per-pixel test against a low-resolution environment depth map, which gives
// stair-stepped edges. Here the video's visibility is the average of a smooth depth comparison at 9 points around the
// pixel (a ~2 depth-texel radius), so an edge fades over a few texels instead of cutting.
//
// `depth` (set 0, binding 3) is the environment depth, re-projected per eye into screen space by the SDK when
// EnvironmentDepthMode is TEXTURE_ONLY or OCCLUSION. Depths are compared as a distance ratio: with the infinite far
// plane used in VR, NDC depth is z = n/d (reverse-Z) or z = 1 - n/d (standard), so d_env/d_frag needs no near plane.
// Which convention the renderer uses is not documented, so both variants exist (softOcclusionStd = standard).

#include <metaSpatialSdkFragmentBase.glsl>
#include <data/shaders/common.glsl>

#ifndef SOFT_REVERSE_Z
#define SOFT_REVERSE_Z 1
#endif

// Distance ratio env / fragment; > 1 means the real world is behind the panel at this pixel.
float distanceRatio(float zEnv, float zFrag) {
#if SOFT_REVERSE_Z
  return zFrag / max(zEnv, 1e-6);
#else
  return (1.0 - zFrag) / max(1.0 - zEnv, 1e-6);
#endif
}

void main() {
  vec4 video = texture(albedoSampler, vertexOut.albedoCoord);

  vec2 screenUv = gl_FragCoord.xy * g_ViewUniform.renderTargetSize.zw;
  float layer = float(getStereoPassId());
  vec2 texel = 1.0 / vec2(textureSize(depth, 0).xy);

  // Centre plus a 2-texel ring of 8 samples.
  const vec2 taps[9] = vec2[9](
    vec2(0.0, 0.0),
    vec2(2.0, 0.0), vec2(-2.0, 0.0), vec2(0.0, 2.0), vec2(0.0, -2.0),
    vec2(1.4, 1.4), vec2(-1.4, 1.4), vec2(1.4, -1.4), vec2(-1.4, -1.4));

  float visible = 0.0;
  for (int i = 0; i < 9; ++i) {
    float zEnv = texture(depth, vec3(screenUv + taps[i] * texel, layer)).r;
    // Fully visible when the world is >= 4 % farther than the panel, hidden when >= 4 % nearer.
    visible += smoothstep(0.96, 1.04, distanceRatio(zEnv, gl_FragCoord.z));
  }
  visible /= 9.0;

  // Premultiplied: the hidden part leaves the eye buffer transparent, so passthrough shows through.
  outColor = vec4(video.rgb * visible, visible);
}
