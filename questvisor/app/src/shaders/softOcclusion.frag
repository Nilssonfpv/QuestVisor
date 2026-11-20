#version 430
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// Soft depth occlusion, reverse-Z depth convention (z = n/d). Body: softOcclusionBody.glsl.
#define SOFT_REVERSE_Z 1
#include "./softOcclusionBody.glsl"
