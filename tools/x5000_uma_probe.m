#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_param.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static const NSUInteger edge = 2048;
static const NSUInteger payload = 2048 * 2048 * 4;
static const NSUInteger MiB = 1U << 20;
static const NSUInteger maxGroups = 768;
static const NSUInteger dispatchTextures = 36;

typedef struct {
    uint64_t primary;
    uint64_t textures;
    uint64_t commits;
    uint64_t evictions;
    uint64_t rewires;
    uint64_t sameAddressRecommits;
    uint64_t sharedAddressRecommits;
    uint64_t mappingAddressChanges;
    uint64_t mappingTrackingFailures;
} ResidencySnapshot;

static uint64_t counter(const char *name)
{
    uint64_t value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname(name, &value, &size, NULL, 0) != 0) { err(1, "%s", name); }
    if (size != sizeof(value)) { errx(1, "Unexpected counter size for %s", name); }
    return value;
}

static BOOL enabled(void)
{
    uint32_t active = 0;
    size_t size = sizeof(active);
    if (sysctlbyname("debug.nred_uma.active", &active, &size, NULL, 0) != 0) {
        if (errno == ENOENT) { return NO; }
        err(1, "debug.nred_uma.active");
    }
    if (size != sizeof(active) || active > 1) { errx(1, "Unexpected debug.nred_uma.active value"); }
    return active == 1;
}

static ResidencySnapshot snapshot(void)
{
    return (ResidencySnapshot){
        counter("debug.nred_uma.bytes_live"),
        counter("debug.nred_uma.textures"),
        counter("debug.nred_uma.task_commits"),
        counter("debug.nred_uma.unwire_count"),
        counter("debug.nred_uma.rewire_count"),
        counter("debug.nred_uma.same_va_recommits"),
        counter("debug.nred_uma.shared_va_recommits"),
        counter("debug.nred_uma.mapping_va_changes"),
        counter("debug.nred_uma.mapping_tracking_failures"),
    };
}

// Size of the firmware carve-out, from the VRAM,totalMB property NootedRed publishes on the GPU. Falls back to 512 MiB.
static uint64_t carveOutBytes(id<MTLDevice> device)
{
    uint64_t bytes = 512 * (uint64_t)MiB;
    const io_registry_entry_t entry =
        IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(device.registryID));
    if (entry == IO_OBJECT_NULL) { return bytes; }
    const CFTypeRef value = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, CFSTR("VRAM,totalMB"),
                                                            kCFAllocatorDefault,
                                                            kIORegistryIterateRecursively | kIORegistryIterateParents);
    int64_t megabytes = 0;
    if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()
        && CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &megabytes) && megabytes > 0) {
        bytes = (uint64_t)megabytes * MiB;
    }
    if (value != NULL) { CFRelease(value); }
    IOObjectRelease(entry);
    return bytes;
}

static uint64_t metalField(id<MTLDevice> device, const char *name)
{
    Ivar field = class_getInstanceVariable(object_getClass(device), name);
    if (field == NULL || strcmp(ivar_getTypeEncoding(field), @encode(uint64_t)) != 0) {
        errx(1, "Could not inspect Metal field %s for %s", name, device.name.UTF8String);
    }
    uint64_t value = 0;
    const unsigned char *object = (__bridge const void *)device;
    memcpy(&value, object + ivar_getOffset(field), sizeof(value));
    return value;
}

static void verifyBudget(id<MTLDevice> device)
{
    const uint64_t workingSet = counter("debug.nred_uma.metal_working_set");
    const uint64_t wired = counter("debug.nred_uma.wired_budget");
    const uint64_t segment = metalField(device, "_segmentByteThreshold");
    printf("Wired budget %llu MiB, Metal working set %llu MiB, segment %llu MiB, dedicated %llu MiB\n",
           (unsigned long long)(wired / MiB), (unsigned long long)(workingSet / MiB),
           (unsigned long long)(segment / MiB), (unsigned long long)(metalField(device, "_videoRam") / MiB));
    if (counter("debug.nred_uma.metal_budget_pages") < 2 || counter("debug.nred_uma.metal_budget_queries") == 0
        || workingSet == 0 || workingSet > wired || device.recommendedMaxWorkingSetSize != workingSet
        || segment != workingSet / 2) {
        errx(1, "The Metal device is not using the advertised pageable memory budget");
    }
}

static void finish(id<MTLCommandBuffer> command)
{
    if (command == nil) { errx(1, "Could not create a command buffer"); }
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        (void)completed;
        dispatch_semaphore_signal(done);
    }];
    [command commit];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC)) != 0) {
        errx(1, "GPU command timed out");
    }
    if (command.status != MTLCommandBufferStatusCompleted) {
        errx(1, "GPU command failed: %s", command.error.localizedDescription.UTF8String ?: "unknown error");
    }
}

static void encodeClear(id<MTLCommandBuffer> command, id<MTLTexture> texture, const uint8_t sample)
{
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassColorAttachmentDescriptor *color = pass.colorAttachments[0];
    color.texture = texture;
    color.loadAction = MTLLoadActionClear;
    color.storeAction = MTLStoreActionStore;
    const double channel = (double)sample / 255.0;
    color.clearColor = MTLClearColorMake(channel, channel, channel, channel);
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (encoder == nil) { errx(1, "Could not create a render encoder"); }
    [encoder endEncoding];
}

static void writeResources(id<MTLCommandQueue> queue, id<MTLBuffer> buffer, id<MTLTexture> texture, const uint8_t sample)
{
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (blit == nil) { errx(1, "Could not create a blit encoder"); }
    [blit fillBuffer:buffer range:NSMakeRange(0, payload) value:sample];
    [blit endEncoding];
    encodeClear(command, texture, sample);
    finish(command);
}

static void verifyBytes(const uint8_t *bytes, const NSUInteger length, const uint8_t sample,
                        const char *label, const NSUInteger index)
{
    for (NSUInteger byte = 0; byte < length; ++byte) {
        if (bytes[byte] != sample) {
            errx(1, "%s %lu byte %lu: expected %u, got %u", label, (unsigned long)index, (unsigned long)byte,
                 (unsigned)sample, (unsigned)bytes[byte]);
        }
    }
}

static void verify(id<MTLBuffer> readback, const NSUInteger offset, const NSUInteger length,
                   const uint8_t sample, const char *label, const NSUInteger index)
{
    verifyBytes((const uint8_t *)readback.contents + offset, length, sample, label, index);
}

static uint8_t resourceSample(const NSUInteger seed, const NSUInteger index)
{
    return (uint8_t)((index + seed - 1) % UINT8_MAX + 1);
}

static void encodeTextureCopy(id<MTLBlitCommandEncoder> blit, id<MTLTexture> texture, id<MTLBuffer> readback,
                              const NSUInteger offset)
{
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(edge, edge, 1) toBuffer:readback destinationOffset:offset
    destinationBytesPerRow:edge * 4 destinationBytesPerImage:payload];
}

static void readResources(id<MTLCommandQueue> queue, id<MTLBuffer> buffer, id<MTLTexture> texture,
                          id<MTLBuffer> readback, const uint8_t sample, const NSUInteger index)
{
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (blit == nil) { errx(1, "Could not create a readback encoder"); }
    [blit copyFromBuffer:buffer sourceOffset:0 toBuffer:readback destinationOffset:0 size:payload];
    encodeTextureCopy(blit, texture, readback, payload);
    [blit endEncoding];
    finish(command);
    verify(readback, 0, payload, sample, "buffer", index);
    verify(readback, payload, payload, sample, "render target", index);
}

static void verifyPurgeableResources(id<MTLCommandQueue> queue, id<MTLBuffer> buffer, id<MTLTexture> texture,
                                     id<MTLBuffer> readback, const uint8_t sample, const BOOL managed,
                                     const BOOL baseline)
{
    const uint64_t address = buffer.gpuAddress;
    const uint64_t purgeUnwires = baseline ? 0 : counter("debug.nred_uma.purge_unwires");
    NSArray<id<MTLResource>> *resources = @[buffer, texture];
    BOOL statesValid = YES;
    BOOL preserved = YES;
    for (NSUInteger index = 0; index < resources.count; ++index) {
        id<MTLResource> resource = resources[index];
        const MTLPurgeableState before = [resource setPurgeableState:MTLPurgeableStateVolatile];
        const MTLPurgeableState current = [resource setPurgeableState:MTLPurgeableStateKeepCurrent];
        const MTLPurgeableState restored = [resource setPurgeableState:MTLPurgeableStateNonVolatile];
        printf("%s volatility: previous=%lu current=%lu restored=%lu\n", index == 0 ? "Buffer" : "Texture",
               (unsigned long)before, (unsigned long)current, (unsigned long)restored);
        const BOOL volatileState = (current == MTLPurgeableStateVolatile || current == MTLPurgeableStateEmpty)
                                && (restored == MTLPurgeableStateVolatile || restored == MTLPurgeableStateEmpty);
        const BOOL retainedRenderTarget = !managed && index == 1 && current == MTLPurgeableStateNonVolatile
                                       && restored == MTLPurgeableStateNonVolatile;
        statesValid &= before == MTLPurgeableStateNonVolatile && (volatileState || retainedRenderTarget);
        preserved &= restored != MTLPurgeableStateEmpty;
    }
    if (preserved) {
        readResources(queue, buffer, texture, readback, sample, 0);
    }
    if (buffer.gpuAddress != address) { errx(1, "Volatile buffer changed GPU virtual address"); }

    for (NSUInteger index = 0; index < resources.count; ++index) {
        id<MTLResource> resource = resources[index];
        const MTLPurgeableState before = [resource setPurgeableState:MTLPurgeableStateEmpty];
        const MTLPurgeableState current = [resource setPurgeableState:MTLPurgeableStateKeepCurrent];
        const MTLPurgeableState restored = [resource setPurgeableState:MTLPurgeableStateNonVolatile];
        printf("%s discard: previous=%lu current=%lu restored=%lu\n", index == 0 ? "Buffer" : "Texture",
               (unsigned long)before, (unsigned long)current, (unsigned long)restored);
        statesValid &= before == MTLPurgeableStateNonVolatile && current == MTLPurgeableStateEmpty
                       && restored == MTLPurgeableStateEmpty;
    }
    const uint8_t replacement = sample ^ 0xA7;
    writeResources(queue, buffer, texture, replacement);
    readResources(queue, buffer, texture, readback, replacement, 0);
    if (buffer.gpuAddress != address) { errx(1, "Purgeable buffer changed GPU virtual address"); }
    puts("GPU refill and readback passed at the original GPU virtual address");
    if (!statesValid) { errx(1, "Metal did not report the requested purgeable states"); }
    if (!baseline && counter("debug.nred_uma.purge_unwires") <= purgeUnwires) {
        errx(1, "The purgeable resources did not exercise idle backing release");
    }
}

static void uploadManagedResources(id<MTLBuffer> buffer, id<MTLTexture> texture, id<MTLBuffer> upload,
                                   const uint8_t sample)
{
    memset(buffer.contents, sample, payload);
    [buffer didModifyRange:NSMakeRange(0, payload)];
    memset(upload.contents, sample, payload);
    [texture replaceRegion:MTLRegionMake2D(0, 0, edge, edge) mipmapLevel:0 withBytes:upload.contents
               bytesPerRow:edge * 4];
}

static void verifyManagedReadback(id<MTLCommandQueue> queue, id<MTLBuffer> buffer, id<MTLTexture> texture,
                                   id<MTLBuffer> readback, const uint8_t sample, const NSUInteger index)
{
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (blit == nil) { errx(1, "Could not create a managed-resource synchronization encoder"); }
    [blit synchronizeResource:buffer];
    [blit synchronizeResource:texture];
    [blit endEncoding];
    finish(command);

    verifyBytes(buffer.contents, payload, sample, "managed CPU buffer", index);
    [texture getBytes:readback.contents bytesPerRow:edge * 4 fromRegion:MTLRegionMake2D(0, 0, edge, edge) mipmapLevel:0];
    verify(readback, 0, payload, sample, "managed CPU texture", index);
}

static void verifySingleDispatch(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                 NSArray<id<MTLTexture>> *textures, id<MTLBuffer> readback, const NSUInteger seed)
{
    NSString *source = [NSString stringWithFormat:
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "kernel void sampleTextures(array<texture2d<float>, %lu> textures [[texture(0)]],\n"
         "                           device uchar4 *output [[buffer(0)]],\n"
         "                           uint pixel [[thread_position_in_grid]]) {\n"
         "    uint side = pixel & 1;\n"
         "    uint index = pixel >> 1;\n"
         "    uint2 corner = side ? uint2(%lu, %lu) : uint2(0);\n"
         "    output[pixel] = uchar4(round(textures[index].read(corner) * 255.0f));\n"
         "}\n", (unsigned long)dispatchTextures, (unsigned long)(edge - 1), (unsigned long)(edge - 1)];
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    if (library == nil) { errx(1, "Could not compile the texture sampler: %s", error.localizedDescription.UTF8String); }
    id<MTLFunction> function = [library newFunctionWithName:@"sampleTextures"];
    if (function == nil) { errx(1, "Texture sampler was not found"); }
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (pipeline == nil) { errx(1, "Could not build the texture sampler: %s", error.localizedDescription.UTF8String); }

    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
    if (compute == nil) { errx(1, "Could not create texture sampler encoder"); }
    [compute setComputePipelineState:pipeline];
    [compute setBuffer:readback offset:0 atIndex:0];
    for (NSUInteger index = 0; index < dispatchTextures; ++index) {
        [compute setTexture:textures[index] atIndex:index];
    }
    [compute dispatchThreads:MTLSizeMake(dispatchTextures * 2, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(pipeline.threadExecutionWidth, 1, 1)];
    [compute endEncoding];
    finish(command);
    for (NSUInteger index = 0; index < dispatchTextures; ++index) {
        verify(readback, index * 8, 8, resourceSample(seed, index), "single-dispatch render target", index);
    }
    printf("Single dispatch verified both corners of %lu textures totaling %lu MiB\n",
           (unsigned long)dispatchTextures, (unsigned long)(dispatchTextures * payload / MiB));
}

static id<MTLTexture> newSurfaceTexture(id<MTLDevice> device, IOSurfaceRef surface)
{
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                   width:edge height:edge mipmapped:NO];
    desc.storageMode = MTLStorageModeManaged;
    desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:desc iosurface:surface plane:0];
    if (texture == nil) { errx(1, "Could not import IOSurface texture"); }
    return texture;
}

static void readSurface(id<MTLCommandQueue> queue, id<MTLTexture> texture, id<MTLBuffer> readback,
                        id<MTLTexture> synchronize, const uint8_t expected, const char *label)
{
    id<MTLCommandBuffer> read = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [read blitCommandEncoder];
    if (blit == nil) { errx(1, "Could not create IOSurface readback encoder"); }
    encodeTextureCopy(blit, texture, readback, 0);
    if (synchronize != nil) { [blit synchronizeResource:synchronize]; }
    [blit endEncoding];
    finish(read);
    verify(readback, 0, payload, expected, label, 0);
}

static void releaseRegisteredPorts(mach_port_array_t ports, const mach_msg_type_number_t count)
{
    for (mach_msg_type_number_t index = 0; index < count; ++index) {
        if (MACH_PORT_VALID(ports[index])) { mach_port_deallocate(mach_task_self(), ports[index]); }
    }
    vm_deallocate(mach_task_self(), (vm_address_t)ports, count * sizeof(*ports));
}

typedef struct {
    pid_t pid;
    int socket;
} SurfacePeer;

static void signalSurfacePeer(const int socket)
{
    const uint8_t signal = 1;
    ssize_t sent;
    do { sent = send(socket, &signal, sizeof(signal), 0); } while (sent < 0 && errno == EINTR);
    if (sent != sizeof(signal)) { err(1, "Could not signal IOSurface peer"); }
}

static void awaitSurfacePeer(const int socket)
{
    uint8_t signal = 0;
    ssize_t received;
    do { received = recv(socket, &signal, sizeof(signal), 0); } while (received < 0 && errno == EINTR);
    if (received < 0) { err(1, "Could not receive IOSurface peer signal"); }
    if (received != sizeof(signal) || signal != 1) { errx(1, "IOSurface peer closed before completing verification"); }
}

static void verifySurfacePeer(const mach_msg_type_number_t slot, const int socket)
{
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    if (mach_ports_lookup(mach_task_self(), &ports, &count) != KERN_SUCCESS || slot >= count) {
        errx(1, "IOSurface port did not arrive in the peer");
    }
    IOSurfaceRef surface = IOSurfaceLookupFromMachPort(ports[slot]);
    releaseRegisteredPorts(ports, count);
    if (surface == NULL) { errx(1, "Peer could not look up IOSurface"); }
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> readback = [device newBufferWithLength:payload options:MTLResourceStorageModeShared];
    if (queue == nil || readback == nil) { errx(1, "Could not initialize peer Metal device"); }
    id<MTLTexture> texture = newSurfaceTexture(device, surface);
    CFRelease(surface);
    readSurface(queue, texture, readback, nil, 197, "Peer GPU readback before the workload");
    signalSurfacePeer(socket);
    awaitSurfacePeer(socket);
    readSurface(queue, texture, readback, nil, 197, "Peer GPU readback after the workload");
    id<MTLCommandBuffer> write = [queue commandBuffer];
    encodeClear(write, texture, 139);
    finish(write);
    signalSurfacePeer(socket);
    awaitSurfacePeer(socket);
    readSurface(queue, texture, readback, nil, 83, "Peer GPU readback after producer CPU reupload");
    close(socket);
    printf("IOSurface peer %d verified the shared texture across the workload and wrote its update\n", getpid());
}

static SurfacePeer startSurfacePeer(const char *program, IOSurfaceRef surface, const unsigned displayWait)
{
    int endpoints[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, endpoints) != 0) { err(1, "socketpair"); }
    const int noSigpipe = 1;
    const struct timeval timeout = {240 + displayWait, 0};
    for (size_t index = 0; index < sizeof(endpoints) / sizeof(*endpoints); ++index) {
        if (setsockopt(endpoints[index], SOL_SOCKET, SO_NOSIGPIPE, &noSigpipe, sizeof(noSigpipe)) != 0
            || setsockopt(endpoints[index], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
            err(1, "Could not configure IOSurface peer socket");
        }
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0
        || posix_spawn_file_actions_addclose(&actions, endpoints[0]) != 0) {
        errx(1, "Could not configure IOSurface peer file actions");
    }

    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    if (mach_ports_lookup(mach_task_self(), &ports, &count) != KERN_SUCCESS || count != TASK_PORT_REGISTER_MAX) {
        errx(1, "Could not inspect inheritable Mach ports");
    }
    mach_msg_type_number_t slot = 0;
    while (slot < count && ports[slot] != MACH_PORT_NULL) { ++slot; }
    if (slot == count) { errx(1, "No free inheritable Mach port slot"); }

    const mach_port_t surfacePort = IOSurfaceCreateMachPort(surface);
    if (!MACH_PORT_VALID(surfacePort)) { errx(1, "Could not create IOSurface Mach port"); }
    ports[slot] = surfacePort;
    if (mach_ports_register(mach_task_self(), ports, count) != KERN_SUCCESS) {
        errx(1, "Could not register IOSurface port for peer");
    }
    char indexText[16];
    char socketText[16];
    snprintf(indexText, sizeof(indexText), "%u", slot);
    snprintf(socketText, sizeof(socketText), "%d", endpoints[1]);
    char *const arguments[] = {(char *)program, "--surface-peer", indexText, socketText, NULL};
    pid_t peer = 0;
    const int spawnError = posix_spawnp(&peer, program, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(endpoints[1]);
    ports[slot] = MACH_PORT_NULL;
    const kern_return_t restored = mach_ports_register(mach_task_self(), ports, count);
    mach_port_deallocate(mach_task_self(), surfacePort);
    releaseRegisteredPorts(ports, count);
    if (restored != KERN_SUCCESS || spawnError != 0) { errx(1, "Could not launch IOSurface peer"); }
    awaitSurfacePeer(endpoints[0]);
    printf("IOSurface %u mapped by producer %d and peer %d before the working set\n",
           IOSurfaceGetID(surface), getpid(), peer);
    fflush(stdout);
    return (SurfacePeer){peer, endpoints[0]};
}

static void finishSurfacePeer(const SurfacePeer peer)
{
    signalSurfacePeer(peer.socket);
    close(peer.socket);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(peer.pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != peer.pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errx(1, "IOSurface peer verification failed");
    }
}

static void uploadSurface(IOSurfaceRef surface, const uint8_t value)
{
    if (IOSurfaceLock(surface, 0, NULL) != kIOReturnSuccess) { errx(1, "Could not lock IOSurface for upload"); }
    memset(IOSurfaceGetBaseAddress(surface), value, payload);
    if (IOSurfaceUnlock(surface, 0, NULL) != kIOReturnSuccess) { errx(1, "Could not unlock IOSurface after upload"); }
}

static IOSurfaceRef createProbeSurface(void)
{
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
        (__bridge NSString *)kIOSurfaceWidth: @(edge),
        (__bridge NSString *)kIOSurfaceHeight: @(edge),
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4,
        (__bridge NSString *)kIOSurfaceBytesPerRow: @(edge * 4),
        (__bridge NSString *)kIOSurfaceAllocSize: @(payload),
    });
    if (surface == NULL) { errx(1, "Could not create IOSurface"); }
    uploadSurface(surface, 73);
    return surface;
}

static void prepareSurface(id<MTLCommandQueue> queue, id<MTLTexture> texture, id<MTLBuffer> readback)
{
    readSurface(queue, texture, readback, nil, 73, "IOSurface CPU upload");
    id<MTLCommandBuffer> write = [queue commandBuffer];
    encodeClear(write, texture, 197);
    finish(write);
}

static void verifySurfaceAlias(id<MTLCommandQueue> queue, id<MTLTexture> texture, id<MTLTexture> alias,
                               id<MTLBuffer> readback, IOSurfaceRef surface, const uint8_t expected)
{
    readSurface(queue, alias, readback, texture, expected, "IOSurface alias");
    if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) {
        errx(1, "Could not lock IOSurface for CPU readback");
    }
    const uint8_t *bytes = IOSurfaceGetBaseAddress(surface);
    for (NSUInteger byte = 0; byte < payload; ++byte) {
        if (bytes[byte] != expected) {
            errx(1, "IOSurface CPU readback byte %lu: expected %u, got %u", (unsigned long)byte,
                 (unsigned)expected, (unsigned)bytes[byte]);
        }
    }
    if (IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) {
        errx(1, "Could not unlock IOSurface after CPU readback");
    }
    uploadSurface(surface, 83);
    readSurface(queue, alias, readback, nil, 83, "IOSurface CPU reupload");
    puts("IOSurface CPU upload, GPU alias, CPU readback and GPU readback after reupload passed");
}

static void verifyHeapDepth(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLBuffer> readback)
{
    MTLTextureDescriptor *colorDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                          width:edge height:edge mipmapped:NO];
    colorDesc.storageMode = MTLStorageModePrivate;
    colorDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    MTLTextureDescriptor *depthDesc = [colorDesc copy];
    depthDesc.pixelFormat = MTLPixelFormatDepth32Float;
    const MTLSizeAndAlign bufferSize =
        [device heapBufferSizeAndAlignWithLength:payload options:MTLResourceStorageModePrivate];
    const MTLSizeAndAlign colorSize = [device heapTextureSizeAndAlignWithDescriptor:colorDesc];
    const MTLSizeAndAlign depthSize = [device heapTextureSizeAndAlignWithDescriptor:depthDesc];
    MTLHeapDescriptor *heapDesc = [MTLHeapDescriptor new];
    heapDesc.storageMode = MTLStorageModePrivate;
    heapDesc.size = bufferSize.size + bufferSize.align + colorSize.size + colorSize.align
                    + depthSize.size + depthSize.align;
    id<MTLHeap> heap = [device newHeapWithDescriptor:heapDesc];
    id<MTLBuffer> buffer = [heap newBufferWithLength:payload options:MTLResourceStorageModePrivate];
    id<MTLTexture> color = [heap newTextureWithDescriptor:colorDesc];
    id<MTLTexture> depth = [heap newTextureWithDescriptor:depthDesc];
    if (buffer == nil || color == nil || depth == nil) { errx(1, "Could not allocate private Metal heap resources"); }

    writeResources(queue, buffer, color, 151);
    const float expectedDepth = 0.625f;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = expectedDepth;
    id<MTLCommandBuffer> write = [queue commandBuffer];
    id<MTLRenderCommandEncoder> render = [write renderCommandEncoderWithDescriptor:pass];
    if (render == nil) { errx(1, "Could not create a depth render encoder"); }
    [render endEncoding];
    finish(write);

    readResources(queue, buffer, color, readback, 151, 0);
    id<MTLCommandBuffer> read = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [read blitCommandEncoder];
    if (blit == nil) { errx(1, "Could not create a depth readback encoder"); }
    encodeTextureCopy(blit, depth, readback, 0);
    [blit endEncoding];
    finish(read);
    const float *depthValues = readback.contents;
    for (NSUInteger pixel = 0; pixel < edge * edge; ++pixel) {
        if (depthValues[pixel] != expectedDepth) {
            errx(1, "Heap depth texel %lu: expected %.3f, got %.3f",
                 (unsigned long)pixel, expectedDepth, depthValues[pixel]);
        }
    }
    puts("GPU readback passed for private Metal heap buffers, color and depth textures");
}

static void verifyMultisampleResolve(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLBuffer> readback)
{
    if (![device supportsTextureSampleCount:4]) { errx(1, "Metal does not support 4x MSAA"); }
    MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                   width:edge height:edge mipmapped:NO];
    desc.storageMode = MTLStorageModePrivate;
    desc.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> resolved = [device newTextureWithDescriptor:desc];
    desc.textureType = MTLTextureType2DMultisample;
    desc.sampleCount = 4;
    id<MTLTexture> color = [device newTextureWithDescriptor:desc];
    desc.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLTexture> depthStencil = [device newTextureWithDescriptor:desc];
    if (resolved == nil || color == nil || depthStencil == nil) { errx(1, "Could not allocate Metal MSAA attachments"); }

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].resolveTexture = resolved;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    const double channel = 173.0 / 255.0;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(channel, channel, channel, channel);
    pass.depthAttachment.texture = depthStencil;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 0.375;
    pass.stencilAttachment.texture = depthStencil;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.clearStencil = 23;
    id<MTLCommandBuffer> write = [queue commandBuffer];
    id<MTLRenderCommandEncoder> render = [write renderCommandEncoderWithDescriptor:pass];
    if (render == nil) { errx(1, "Could not create Metal MSAA render encoder"); }
    [render endEncoding];
    finish(write);
    readSurface(queue, resolved, readback, nil, 173, "4x MSAA resolve");
    puts("GPU readback passed after 4x MSAA resolve with depth/stencil");
}

static unsigned long argument(const char *text, const unsigned long minimum, const unsigned long maximum)
{
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value < minimum || value > maximum) {
        errx(2, "Expected an integer between %lu and %lu, got %s", minimum, maximum, text);
    }
    return value;
}

static void waitForDisplayReconnect(const unsigned seconds)
{
    const CGDirectDisplayID target = CGMainDisplayID();
    const uint32_t vendor = CGDisplayVendorNumber(target);
    const uint32_t model = CGDisplayModelNumber(target);
    const uint32_t serial = CGDisplaySerialNumber(target);
    if (target == kCGNullDirectDisplay || !CGDisplayIsOnline(target) || CGDisplayIsBuiltin(target) || serial == 0) {
        errx(1, "Hotplug validation needs an online external main display with a serial number");
    }

    CGDisplayModeRef initial = CGDisplayCopyDisplayMode(target);
    if (initial == NULL) { errx(1, "Could not read the initial display mode"); }
    const size_t width = CGDisplayModeGetPixelWidth(initial);
    const size_t height = CGDisplayModeGetPixelHeight(initial);
    CGDisplayModeRelease(initial);
    printf("Holding original GPU resources for %zux%zu display %u hotplug for up to %u seconds\n",
           width, height, target, seconds);

    BOOL disconnected = NO;
    const uint64_t deadline = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) + (uint64_t)seconds * NSEC_PER_SEC;
    while (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) < deadline) {
        CGDirectDisplayID displays[32];
        uint32_t count = 0;
        if (CGGetOnlineDisplayList(sizeof(displays) / sizeof(displays[0]), displays, &count) == kCGErrorSuccess) {
            CGDirectDisplayID match = kCGNullDirectDisplay;
            for (uint32_t index = 0; index < count; ++index) {
                const CGDirectDisplayID display = displays[index];
                if (CGDisplayVendorNumber(display) == vendor && CGDisplayModelNumber(display) == model
                    && CGDisplaySerialNumber(display) == serial) {
                    match = display;
                    break;
                }
            }
            if (!disconnected && match == kCGNullDirectDisplay) {
                disconnected = YES;
                puts("External display disconnected while GPU resources were still live");
            }
            if (disconnected && match != kCGNullDirectDisplay && CGDisplayIsActive(match)) {
                CGDisplayModeRef mode = CGDisplayCopyDisplayMode(match);
                if (mode != NULL) {
                    const BOOL restored = CGDisplayModeGetPixelWidth(mode) == width
                                       && CGDisplayModeGetPixelHeight(mode) == height;
                    CGDisplayModeRelease(mode);
                    if (restored) {
                        printf("Original display reconnected at %zux%zu (display %u)\n", width, height, match);
                        return;
                    }
                }
            }
        }
        [NSThread sleepForTimeInterval:0.2];
    }
    if (!disconnected) { errx(1, "No external display disconnect was observed"); }
    errx(1, "The external display did not reconnect in its original mode");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    @autoreleasepool {
        if (argc == 4 && strcmp(argv[1], "--surface-peer") == 0) {
            verifySurfacePeer((mach_msg_type_number_t)argument(argv[2], 0, TASK_PORT_REGISTER_MAX - 1),
                              (int)argument(argv[3], 3, INT_MAX));
            return 0;
        }
        BOOL baseline = NO;
        BOOL requireReclaim = NO;
        BOOL requireBudget = NO;
        BOOL requireSurfaces = NO;
        BOOL crossProcess = NO;
        BOOL layouts = NO;
        BOOL managed = NO;
        BOOL purgeable = NO;
        BOOL singleDispatch = NO;
        NSUInteger groups = 20;
        NSUInteger seed = 1;
        unsigned idleSeconds = 0;
        unsigned hotplugSeconds = 0;
        for (int index = 1; index < argc; ++index) {
            if (strcmp(argv[index], "--baseline") == 0) {
                baseline = YES;
            } else if (strcmp(argv[index], "--require-reclaim") == 0) {
                requireReclaim = YES;
            } else if (strcmp(argv[index], "--require-budget") == 0) {
                requireBudget = YES;
            } else if (strcmp(argv[index], "--require-surfaces") == 0) {
                requireSurfaces = YES;
            } else if (strcmp(argv[index], "--surface-ipc") == 0) {
                crossProcess = YES;
            } else if (strcmp(argv[index], "--layouts") == 0) {
                layouts = YES;
            } else if (strcmp(argv[index], "--managed") == 0) {
                managed = YES;
            } else if (strcmp(argv[index], "--purgeable") == 0) {
                purgeable = YES;
            } else if (strcmp(argv[index], "--single-dispatch") == 0) {
                singleDispatch = YES;
            } else if (strcmp(argv[index], "--groups") == 0 && index + 1 < argc) {
                groups = argument(argv[++index], 1, maxGroups);
            } else if (strcmp(argv[index], "--seed") == 0 && index + 1 < argc) {
                seed = argument(argv[++index], 1, UINT8_MAX);
            } else if (strcmp(argv[index], "--idle") == 0 && index + 1 < argc) {
                idleSeconds = (unsigned)argument(argv[++index], 0, 60);
            } else if (strcmp(argv[index], "--hotplug") == 0 && index + 1 < argc) {
                hotplugSeconds = (unsigned)argument(argv[++index], 1, 600);
            } else {
                errx(2, "Usage: %s [--baseline] [--groups 1-%lu] [--seed 1-255] [--idle 0-60] [--hotplug 1-600] "
                        "[--require-budget] [--require-reclaim] [--require-surfaces] "
                        "[--surface-ipc] [--layouts] [--managed] [--purgeable] [--single-dispatch]",
                     argv[0], (unsigned long)maxGroups);
            }
        }
        if (baseline && (requireReclaim || requireBudget || requireSurfaces)) {
            errx(2, "Backing, budget and reclaim verification require the unified-memory kext");
        }
        if (singleDispatch && groups < dispatchTextures) {
            errx(2, "--single-dispatch requires at least %lu resource groups", (unsigned long)dispatchTextures);
        }
        const BOOL active = enabled();
        if (baseline && active) { errx(1, "--baseline requires a boot with unified memory off"); }
        if (!baseline && !active) { errx(1, "debug.nred_uma.active is not 1"); }

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> readback = [device newBufferWithLength:payload * 2 options:MTLResourceStorageModeShared];
        if (queue == nil || readback == nil) { errx(1, "Could not initialize Metal and shared readback storage"); }
        printf("%s, %lu MiB of %s buffers and render targets, API working set %llu MiB\n",
               device.name.UTF8String, (unsigned long)(groups * payload * 2 / MiB),
               managed ? "managed" : "private",
               (unsigned long long)(device.recommendedMaxWorkingSetSize / MiB));
        const uint64_t carveOut = carveOutBytes(device);
        printf("Carve-out %llu MiB\n", (unsigned long long)(carveOut / MiB));
        if (!baseline && groups > 1 && groups * payload * 2 <= carveOut) {
            printf("The working set fits in the carve-out; use more than %llu groups to test beyond it\n",
                   (unsigned long long)(carveOut / (payload * 2)));
        }
        printf("First resource sample %lu\n", (unsigned long)seed);
        if (requireBudget) { verifyBudget(device); }
        fflush(stdout);

        const ResidencySnapshot before = baseline ? (ResidencySnapshot){0} : snapshot();
        const uint64_t surfaceCount = requireSurfaces ? counter("debug.nred_uma.surfaces") : 0;
        IOSurfaceRef surface = createProbeSurface();
        id<MTLTexture> surfaceTexture = newSurfaceTexture(device, surface);
        id<MTLTexture> surfaceAlias = newSurfaceTexture(device, surface);
        prepareSurface(queue, surfaceTexture, readback);
        const SurfacePeer peer = crossProcess ? startSurfacePeer(argv[0], surface, hotplugSeconds)
                                              : (SurfacePeer){0, -1};
        if (requireSurfaces) {
            const uint64_t surfaceGrowth = counter("debug.nred_uma.surfaces") - surfaceCount;
            printf("Pageable IOSurface caches +%llu\n", (unsigned long long)surfaceGrowth);
            if (surfaceGrowth == 0) { errx(1, "No pageable IOSurface cache was observed"); }
        }

        __attribute__((objc_precise_lifetime)) NSMutableArray<id<MTLBuffer>> *buffers =
            [NSMutableArray arrayWithCapacity:groups];
        __attribute__((objc_precise_lifetime)) NSMutableArray<id<MTLTexture>> *textures =
            [NSMutableArray arrayWithCapacity:groups];
        NSMutableArray<NSNumber *> *addresses = [NSMutableArray arrayWithCapacity:groups];
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                       width:edge height:edge mipmapped:NO];
        desc.storageMode = managed ? MTLStorageModeManaged : MTLStorageModePrivate;
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        const MTLResourceOptions bufferOptions = managed ? MTLResourceStorageModeManaged : MTLResourceStorageModePrivate;
        for (NSUInteger index = 0; index < groups; ++index) {
            @autoreleasepool {
                id<MTLBuffer> buffer = [device newBufferWithLength:payload options:bufferOptions];
                id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
                if (buffer == nil || texture == nil) { errx(1, "Allocation failed at group %lu", (unsigned long)index); }
                const uint8_t sample = resourceSample(seed, index);
                if (managed) {
                    uploadManagedResources(buffer, texture, readback, sample ^ 0x35);
                    readResources(queue, buffer, texture, readback, sample ^ 0x35, index);
                }
                writeResources(queue, buffer, texture, sample);
                [buffers addObject:buffer];
                [textures addObject:texture];
                [addresses addObject:@(buffer.gpuAddress)];
            }
        }

        if (idleSeconds != 0) {
            printf("Holding %lu resource pairs for %u seconds before verification\n",
                   (unsigned long)groups, idleSeconds);
            fflush(stdout);
            sleep(idleSeconds);
        }
        if (hotplugSeconds != 0) { waitForDisplayReconnect(hotplugSeconds); }
        if (singleDispatch) { verifySingleDispatch(device, queue, textures, readback, seed); }
        for (NSUInteger index = 0; index < groups; ++index) {
            @autoreleasepool {
                id<MTLBuffer> buffer = buffers[index];
                id<MTLTexture> texture = textures[index];
                const uint8_t sample = resourceSample(seed, index);
                readResources(queue, buffer, texture, readback, sample, index);
                if (managed) {
                    verifyManagedReadback(queue, buffer, texture, readback, sample, index);
                    uploadManagedResources(buffer, texture, readback, sample ^ 0xCA);
                    readResources(queue, buffer, texture, readback, sample ^ 0xCA, index);
                }
                const uint64_t originalAddress = addresses[index].unsignedLongLongValue;
                if ((requireReclaim && originalAddress == 0) || buffer.gpuAddress != originalAddress) {
                    errx(1, "GPU virtual address missing or changed for buffer %lu", (unsigned long)index);
                }
                if (requireReclaim) {
                    printf("Reclaimed-workload buffer %lu readback passed at GPU VA 0x%llx\n",
                           (unsigned long)index, (unsigned long long)originalAddress);
                }
            }
        }
        if (seed != 1) {
            printf("First buffer readback passed at GPU VA 0x%llx\n", addresses[0].unsignedLongLongValue);
        }
        if (crossProcess) {
            signalSurfacePeer(peer.socket);
            awaitSurfacePeer(peer.socket);
        }
        verifySurfaceAlias(queue, surfaceTexture, surfaceAlias, readback, surface, crossProcess ? 139 : 197);
        if (crossProcess) { finishSurfacePeer(peer); }
        CFRelease(surface);

        if (!baseline) {
            const ResidencySnapshot after = snapshot();
            const uint64_t primaryGrowth = after.primary > before.primary ? after.primary - before.primary : 0;
            printf("Managed primary growth %llu MiB, textures +%llu, per-task commits +%llu, evictions +%llu, rewires +%llu\n",
                   (unsigned long long)(primaryGrowth / MiB),
                   (unsigned long long)(after.textures - before.textures),
                   (unsigned long long)(after.commits - before.commits),
                   (unsigned long long)(after.evictions - before.evictions),
                   (unsigned long long)(after.rewires - before.rewires));
            printf("Same-address recommits +%llu, shared backing same-address recommits +%llu, "
                   "mapping address changes +%llu, tracking failures +%llu\n",
                   (unsigned long long)(after.sameAddressRecommits - before.sameAddressRecommits),
                   (unsigned long long)(after.sharedAddressRecommits - before.sharedAddressRecommits),
                   (unsigned long long)(after.mappingAddressChanges - before.mappingAddressChanges),
                   (unsigned long long)(after.mappingTrackingFailures - before.mappingTrackingFailures));
            if (after.textures == before.textures || after.commits == before.commits
                || (groups * payload * 2 > carveOut && primaryGrowth <= carveOut)) {
                errx(1, "Insufficient managed primary backing or per-task GPU mappings");
            }
            if (requireReclaim && (after.evictions == before.evictions
                                   || after.sameAddressRecommits == before.sameAddressRecommits
                                   || after.mappingAddressChanges != before.mappingAddressChanges
                                   || after.mappingTrackingFailures != before.mappingTrackingFailures)) {
                errx(1, "Could not verify stable GPU mappings through eviction");
            }
        }
        puts("GPU readback passed for buffers, render targets and IOSurface aliases");
        if (managed) {
            puts("CPU/GPU synchronization and reupload passed for the original Metal managed buffers and textures");
        }
        if (purgeable) {
            const uint8_t sample = resourceSample(seed, 0);
            writeResources(queue, buffers[0], textures[0], sample);
            verifyPurgeableResources(queue, buffers[0], textures[0], readback, sample, managed, baseline);
        }
        if (layouts) {
            verifyHeapDepth(device, queue, readback);
            verifyMultisampleResolve(device, queue, readback);
        }
    }
    return 0;
}
