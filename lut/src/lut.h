/*
 * lut3d: 3D color LUT with trilinear interpolation.
 *
 * Nodes can be spaced uniformly in physical radiance (spacing_law = 1.0) or
 * non-uniformly with power-law spacing x_i = (i/(N-1))^p (spacing_law = p).
 * Uniform lookup is a single FMA index; non-uniform lookup needs a binary
 * search per axis, which is the extra cost quantified by the benchmark.
 */
#ifndef LUT32_H
#define LUT32_H

#define LUT_MAX_N 32

typedef struct {
    int n;                 /* nodes per axis */
    float spacing_law;     /* p: x_i = (i/(N-1))^p; 1.0 == uniform */
    float nodes[LUT_MAX_N];
    /* data[(i*n + j)*n + k][ch], channelled RGB at node (i,j,k) */
    float data[LUT_MAX_N * LUT_MAX_N * LUT_MAX_N * 3];
} Lut3d;

/* fill the node axis (shared by all three channels) */
void lut_set_nodes(Lut3d* lut, int n, float spacing_law);

/* populate data from a channelwise transform f */
typedef void (*LutTransform)(const float in[3], float out[3]);
void lut_bake(Lut3d* lut, LutTransform f);

/* trilinear sample; returns 0 on success, -1 on bad input */
int lut_sample(const Lut3d* lut, const float in[3], float out[3]);

/* uniform-only fast path (FMA index instead of binary search); -1 if
 * the LUT is non-uniform */
int lut_sample_fast(const Lut3d* lut, const float in[3], float out[3]);

/* reference transform the LUT is allowed to approximate (exact evaluation) */
void lut_reference(const float in[3], float out[3]);

/* same curve, scalar form */
float lut_curve(float x);

#endif
