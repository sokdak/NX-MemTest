// GPU pump verification compute shader.
//
// Each invocation processes one uvec4 (16 bytes) of the source and the
// destination buffers, XORs them, OR-reduces the four components into a
// single uint, and atomicOrs that into the result buffer. The CPU resets
// the result word to zero before each dispatch and inspects it after the
// queue drains. Non-zero == at least one byte differed between src and dst.
//
// Workgroup size matches one Maxwell warp x 8 so a 64 MiB buffer
// (= 4M uvec4 elements) dispatches as 16384 workgroups, well within the
// SM's resident-thread budget on Tegra X1.

#version 460

layout (local_size_x = 256) in;

layout (std430, binding = 0) readonly buffer SrcBuffer {
    uvec4 src[];
};

layout (std430, binding = 1) readonly buffer DstBuffer {
    uvec4 dst[];
};

layout (std430, binding = 2) coherent buffer ResultBuffer {
    uint mismatch;
} result;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    uvec4 diff = src[idx] ^ dst[idx];
    uint reduced = diff.x | diff.y | diff.z | diff.w;
    if (reduced != 0u) {
        atomicOr(result.mismatch, 1u);
    }
}
