/*
 * dramtop - GPU bandwidth probe (macOS)
 *
 * Streams reads through a GPU buffer far larger than the system level cache
 * with a Metal compute kernel and reports the best rate, which serves as the
 * GPU's own bandwidth ceiling. The kernel is compiled from source at run time,
 * so building dramtop needs no Metal toolchain.
 *
 * Copyright (c) 2026 Super Zheng (https://github.com/medns). MIT License.
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

double gpu_probe_gbs(unsigned long long max_bytes);

static NSString *const KERNEL = @"#include <metal_stdlib>\n"
                                 "using namespace metal;\n"
                                 "kernel void stream(device const float4 *in [[buffer(0)]], device float *out [[buffer(1)]],\n"
                                 "                   constant uint &n [[buffer(2)]], uint gid [[thread_position_in_grid]],\n"
                                 "                   uint grid [[threads_per_grid]]) {\n"
                                 "    float4 acc = 0;\n"
                                 "    for (uint i = gid; i < n; i += grid) acc += in[i];\n"
                                 "    out[gid] = acc.x + acc.y + acc.z + acc.w;\n"
                                 "}\n";

enum { PASSES = 4, DISPATCHES = 4 };
static const NSUInteger GRID = 1u << 20, GROUP = 256;
static const unsigned long long MAX_BYTES = 512ULL << 20, MIN_BYTES = 64ULL << 20;

double gpu_probe_gbs(unsigned long long max_bytes) {
    @autoreleasepool {
        unsigned long long want = max_bytes < MAX_BYTES ? max_bytes : MAX_BYTES;
        NSUInteger bytes = (NSUInteger)(want & ~0xffffULL);
        if (bytes < MIN_BYTES) return 0;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) return 0;
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:KERNEL options:nil error:&err];
        id<MTLFunction> fn = [lib newFunctionWithName:@"stream"];
        id<MTLComputePipelineState> pso = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
        id<MTLBuffer> in = [dev newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
        id<MTLBuffer> out = [dev newBufferWithLength:GRID * sizeof(float) options:MTLResourceStorageModePrivate];
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        if (!pso || !in || !out || !queue || pso.maxTotalThreadsPerThreadgroup < GROUP) return 0;

        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> fill = [cb blitCommandEncoder];
        [fill fillBuffer:in range:NSMakeRange(0, bytes) value:1];
        [fill endEncoding];
        [cb commit];

        const uint32_t n = (uint32_t)(bytes / 16);
        double best = 0;
        for (int pass = 0; pass < PASSES; pass++) {
            cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:in offset:0 atIndex:0];
            [enc setBuffer:out offset:0 atIndex:1];
            [enc setBytes:&n length:sizeof n atIndex:2];
            for (int d = 0; d < DISPATCHES; d++)
                [enc dispatchThreads:MTLSizeMake(GRID, 1, 1) threadsPerThreadgroup:MTLSizeMake(GROUP, 1, 1)];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            double dt = cb.GPUEndTime - cb.GPUStartTime, gbs = dt > 0 ? (double)bytes * DISPATCHES / dt / 1e9 : 0;
            if (pass > 0 && cb.status == MTLCommandBufferStatusCompleted && gbs > best) best = gbs; /* pass 0 warms up */
        }
        return best;
    }
}
