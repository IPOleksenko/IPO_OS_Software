/* gpu_info.c - Inspect VirtIO-GPU / VirGL hardware status and switch GPU/CPU backend mode. */

#include <ipo/gpu_client.h>
#include <stdio.h>
#include <string.h>

static const char *backend_name(uint32_t m) {
    switch (m) {
        case IPO_GPU_BACKEND_VIRGL_3D:  return "VirGL-3D (Hardware/Virtual GPU)";
        case IPO_GPU_BACKEND_VIRTIO_2D: return "VirtIO-2D (2D DMA Scanout Only)";
        default:                        return "CPU-Fallback (Software Rasterizer)";
    }
}

int main(int argc, char **argv) {
    if (argc >= 2) {
        if (strcmp(argv[1], "cpu") == 0) {
            ipo_gpu_set_mode(1u);
            printf("[gpu_info] Forced CPU rendering fallback enabled.\n");
        } else if (strcmp(argv[1], "gpu") == 0 || strcmp(argv[1], "auto") == 0) {
            ipo_gpu_set_mode(0u);
            printf("[gpu_info] Automatic GPU rendering enabled (VirGL if available, else CPU fallback).\n");
        } else {
            printf("Usage: gpu_info [gpu|auto|cpu]\n");
            return 1;
        }
    }

    ipo_gpu_info_t info;
    if (!ipo_gpu_get_info(&info)) {
        printf("[gpu_info] GPU syscall unavailable.\n");
        return 1;
    }

    printf("=== IPO_OS GPU Subsystem ===\n");
    printf("Backend Mode : %s\n", backend_name(info.backend_mode));
    printf("HW Detected  : %s (%s)\n", info.hw_detected ? "YES" : "NO", info.name);
    if (info.hw_detected) {
        printf("PCI Location : %02x:%02x.%x (%04x:%04x)\n",
               (unsigned)info.pci_bus, (unsigned)info.pci_slot, (unsigned)info.pci_func,
               (unsigned)info.vendor_id, (unsigned)info.device_id);
        printf("VirGL 3D     : %s (capset=%u v%u, scanouts=%u, primary=%s)\n",
               info.virgl_3d_supported ? "YES" : "NO",
               (unsigned)info.capset_id, (unsigned)info.capset_version,
               (unsigned)info.num_scanouts, info.primary_scanout ? "yes" : "no");
        printf("Max Geom     : %ux%u\n", (unsigned)info.max_width, (unsigned)info.max_height);
    }
    printf("Force CPU    : %s\n", info.forced_cpu_fallback ? "YES" : "NO");
    printf("GPU Draws    : %llu\n", (unsigned long long)info.gpu_draw_calls);
    printf("GPU Blits    : %llu\n", (unsigned long long)info.gpu_blits);
    printf("Cmd Batches  : %llu\n", (unsigned long long)info.gpu_cmds_submitted);
    printf("CPU Fallbacks: %llu\n", (unsigned long long)info.cpu_fallbacks);
    return 0;
}
