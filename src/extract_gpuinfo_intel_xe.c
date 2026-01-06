/*
 *
 * Copyright (C) 2022 Maxime Schmitt <maxime.schmitt91@gmail.com>
 *
 * This file is part of Nvtop and adapted from igt-gpu-tools from Intel Corporation.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Intel XPU Manager integration for enhanced telemetry (global GPU utilization,
 * PCIe throughput, memory temperature). Requires libxpum.so from:
 * https://github.com/intel/xpumanager
 *
 */

#include "nvtop/device_discovery.h"
#include "nvtop/extract_gpuinfo_common.h"
#include "nvtop/extract_processinfo_fdinfo.h"
#include "nvtop/time.h"

#include "extract_gpuinfo_intel.h"

#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libdrm/drm.h>
#include <libdrm/xe_drm.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uthash.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

// XPU Manager types and function pointers
typedef int32_t xpum_device_id_t;
typedef int32_t xpum_result_t;
#define XPUM_OK 0
#define XPUM_MAX_STATS 64

typedef enum {
  XPUM_STATS_GPU_UTILIZATION = 0,
  XPUM_STATS_POWER = 4,
  XPUM_STATS_GPU_FREQUENCY = 6,
  XPUM_STATS_GPU_CORE_TEMPERATURE = 7,
  XPUM_STATS_MEMORY_USED = 8,
  XPUM_STATS_ENGINE_GROUP_MEDIA_ALL_UTILIZATION = 16,
  XPUM_STATS_PCIE_READ_THROUGHPUT = 32,
  XPUM_STATS_PCIE_WRITE_THROUGHPUT = 33,
} xpum_stats_type_t;

typedef struct {
  int32_t metricsType;
  uint8_t isCounter;
  uint64_t value;
  uint64_t accumulated;
  uint64_t min;
  uint64_t avg;
  uint64_t max;
  uint32_t scale;
} xpum_device_stats_data_t;

typedef struct {
  int32_t deviceId;
  uint8_t isTileData;
  int32_t tileId;
  int32_t count;
  xpum_device_stats_data_t dataList[XPUM_MAX_STATS];
} xpum_device_stats_t;

static void *xpum_handle = NULL;
static bool xpum_initialized = false;
static xpum_result_t (*xpum_init)(bool) = NULL;
static xpum_result_t (*xpum_shutdown)(void) = NULL;
static xpum_result_t (*xpum_get_stats)(xpum_device_id_t, xpum_device_stats_t[], uint32_t *, uint64_t *, uint64_t *,
                                       uint64_t) = NULL;
static xpum_result_t (*xpum_get_device_id_by_bdf)(const char *, xpum_device_id_t *) = NULL;

static bool xpum_load_library(void) {
  if (xpum_handle)
    return true;

  xpum_handle = dlopen("libxpum.so", RTLD_LAZY);
  if (!xpum_handle)
    return false;

  xpum_init = dlsym(xpum_handle, "xpumInit");
  xpum_shutdown = dlsym(xpum_handle, "xpumShutdown");
  xpum_get_stats = dlsym(xpum_handle, "xpumGetStats");
  xpum_get_device_id_by_bdf = dlsym(xpum_handle, "xpumGetDeviceIdByBDF");

  if (!xpum_init || !xpum_shutdown || !xpum_get_stats || !xpum_get_device_id_by_bdf) {
    dlclose(xpum_handle);
    xpum_handle = NULL;
    return false;
  }

  if (xpum_init(false) != XPUM_OK) {
    dlclose(xpum_handle);
    xpum_handle = NULL;
    return false;
  }

  xpum_initialized = true;
  return true;
}

void gpuinfo_intel_xe_xpum_shutdown(void) {
  if (xpum_initialized && xpum_shutdown) {
    xpum_shutdown();
    xpum_initialized = false;
  }
  if (xpum_handle) {
    dlclose(xpum_handle);
    xpum_handle = NULL;
  }
}

bool gpuinfo_intel_xe_get_xpum_device_id(const char *pci_bdf, int *device_id) {
  if (!xpum_load_library() || !xpum_get_device_id_by_bdf)
    return false;

  xpum_device_id_t dev_id;
  if (xpum_get_device_id_by_bdf(pci_bdf, &dev_id) != XPUM_OK)
    return false;

  *device_id = (int)dev_id;
  return true;
}

static uint64_t xpum_get_stat(xpum_device_stats_t *stats, xpum_stats_type_t type) {
  for (int i = 0; i < stats->count && i < XPUM_MAX_STATS; i++) {
    if (stats->dataList[i].metricsType == (int32_t)type) {
      uint64_t val = stats->dataList[i].value;
      uint32_t scale = stats->dataList[i].scale;
      return scale > 1 ? val / scale : val;
    }
  }
  return 0;
}

static void xpum_refresh_dynamic_info(struct gpuinfo_dynamic_info *dynamic_info, int xpum_device_id) {
  if (!xpum_handle || !xpum_get_stats || xpum_device_id < 0)
    return;

  xpum_device_stats_t stats_list[8];
  memset(stats_list, 0, sizeof(stats_list));
  uint32_t count = 8;
  uint64_t begin, end;

  if (xpum_get_stats(xpum_device_id, stats_list, &count, &begin, &end, 0) != XPUM_OK || count == 0)
    return;

  for (uint32_t i = 0; i < count; i++) {
    xpum_device_stats_t *stats = &stats_list[i];
    if (stats->isTileData)
      continue;

    uint64_t val;

    val = xpum_get_stat(stats, XPUM_STATS_GPU_UTILIZATION);
    if (val > 0)
      SET_GPUINFO_DYNAMIC(dynamic_info, gpu_util_rate, (unsigned)val);

    val = xpum_get_stat(stats, XPUM_STATS_POWER);
    if (val > 0)
      SET_GPUINFO_DYNAMIC(dynamic_info, power_draw, (unsigned)(val * 1000));

    val = xpum_get_stat(stats, XPUM_STATS_GPU_FREQUENCY);
    if (val > 0)
      SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed, (unsigned)val);

    val = xpum_get_stat(stats, XPUM_STATS_GPU_CORE_TEMPERATURE);
    if (val > 0)
      SET_GPUINFO_DYNAMIC(dynamic_info, gpu_temp, (unsigned)val);

    val = xpum_get_stat(stats, XPUM_STATS_MEMORY_USED);
    if (val > 0)
      SET_GPUINFO_DYNAMIC(dynamic_info, used_memory, val);

    val = xpum_get_stat(stats, XPUM_STATS_ENGINE_GROUP_MEDIA_ALL_UTILIZATION);
    if (val > 0) {
      SET_GPUINFO_DYNAMIC(dynamic_info, encoder_rate, (unsigned)val);
      SET_GPUINFO_DYNAMIC(dynamic_info, decoder_rate, (unsigned)val);
    }

    val = xpum_get_stat(stats, XPUM_STATS_PCIE_READ_THROUGHPUT);
    if (val > 0)
      SET_GPUINFO_DYNAMIC(dynamic_info, pcie_rx, (unsigned)(val * 1024 / 1000));

    val = xpum_get_stat(stats, XPUM_STATS_PCIE_WRITE_THROUGHPUT);
    if (val > 0)
      SET_GPUINFO_DYNAMIC(dynamic_info, pcie_tx, (unsigned)(val * 1024 / 1000));
  }
}

// Copied from https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/intel/common/intel_gem.h
static inline int intel_ioctl(int fd, unsigned long request, void *arg) {
  int ret;

  do {
    ret = ioctl(fd, request, arg);
  } while (ret == -1 && (errno == EINTR || errno == EAGAIN));
  return ret;
}

// Copied from https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/intel/common/xe/intel_device_query.c
static void *xe_device_query_alloc_fetch(int fd, uint32_t query_id, uint32_t *len) {
  struct drm_xe_device_query query = {
      .query = query_id,
  };
  if (intel_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query))
    return NULL;

  void *data = calloc(1, query.size);
  if (!data)
    return NULL;

  query.data = (uintptr_t)data;
  if (intel_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query)) {
    free(data);
    return NULL;
  }

  if (len)
    *len = query.size;
  return data;
}

void gpuinfo_intel_xe_refresh_dynamic_info(struct gpu_info *_gpu_info) {
  struct gpu_info_intel *gpu_info = container_of(_gpu_info, struct gpu_info_intel, base);
  struct gpuinfo_dynamic_info *dynamic_info = &gpu_info->base.dynamic_info;

  // Supplement with XPU Manager data (global GPU util, PCIe throughput, mem temp)
  xpum_refresh_dynamic_info(dynamic_info, gpu_info->xpum_device_id);

  if (gpu_info->card_fd) {
    uint32_t length = 0;
    struct drm_xe_query_mem_regions *regions =
        xe_device_query_alloc_fetch(gpu_info->card_fd, DRM_XE_DEVICE_QUERY_MEM_REGIONS, &length);
    if (regions) {
      for (unsigned i = 0; i < regions->num_mem_regions; i++) {
        struct drm_xe_mem_region mr = regions->mem_regions[i];
        // ARC will have VRAM and SYSMEM, integrated graphics will have only one SYSMEM region
        if (mr.mem_class == DRM_XE_MEM_REGION_CLASS_VRAM || regions->num_mem_regions == 1) {
          SET_GPUINFO_DYNAMIC(dynamic_info, total_memory, mr.total_size);
          // xe will report 0 kb used if we don't have CAP_PERFMON
          if (mr.used != 0) {
            SET_GPUINFO_DYNAMIC(dynamic_info, used_memory, mr.used);
            SET_GPUINFO_DYNAMIC(dynamic_info, free_memory, dynamic_info->total_memory - dynamic_info->used_memory);
            SET_GPUINFO_DYNAMIC(dynamic_info, mem_util_rate, dynamic_info->used_memory * 100 / dynamic_info->total_memory);
          }
          break;
        }
      }
      free(regions);
    }
  }
}

static const char xe_drm_intel_vram[] = "drm-total-vram0";
// Render
static const char xe_drm_intel_cycles_rcs[] = "drm-cycles-rcs";
static const char xe_drm_intel_total_cycles_rcs[] = "drm-total-cycles-rcs";
// Video Decode
static const char xe_drm_intel_cycles_vcs[] = "drm-cycles-vcs";
static const char xe_drm_intel_total_cycles_vcs[] = "drm-total-cycles-vcs";
// Video Enhance
static const char xe_drm_intel_cycles_vecs[] = "drm-cycles-vecs";
static const char xe_drm_intel_total_cycles_vecs[] = "drm-total-cycles-vecs";
// Copy
static const char xe_drm_intel_cycles_bcs[] = "drm-cycles-bcs";
static const char xe_drm_intel_total_cycles_bcs[] = "drm-total-cycles-bcs";
// Compute
static const char xe_drm_intel_cycles_ccs[] = "drm-cycles-ccs";
static const char xe_drm_intel_total_cycles_ccs[] = "drm-total-cycles-ccs";

static const char *cycles_keys[] = {xe_drm_intel_cycles_rcs, xe_drm_intel_cycles_vcs, xe_drm_intel_cycles_vecs,
                                    xe_drm_intel_cycles_bcs, xe_drm_intel_cycles_ccs};

static const char *total_cycles_keys[] = {xe_drm_intel_total_cycles_rcs, xe_drm_intel_total_cycles_vcs,
                                          xe_drm_intel_total_cycles_vecs, xe_drm_intel_total_cycles_bcs,
                                          xe_drm_intel_total_cycles_ccs};

bool parse_drm_fdinfo_intel_xe(struct gpu_info *info, FILE *fdinfo_file, struct gpu_process *process_info) {
  struct gpu_info_intel *gpu_info = container_of(info, struct gpu_info_intel, base);
  static char *line = NULL;
  static size_t line_buf_size = 0;
  ssize_t count = 0;

  bool client_id_set = false;
  unsigned cid;
  nvtop_time current_time;
  nvtop_get_current_time(&current_time);

  union intel_cycles gpu_cycles = {.array = {0}};
  union intel_cycles total_cycles = {.array = {0}};

  while ((count = getline(&line, &line_buf_size, fdinfo_file)) != -1) {
    char *key, *val;
    if (line[count - 1] == '\n') {
      line[--count] = '\0';
    }

    if (!extract_drm_fdinfo_key_value(line, &key, &val))
      continue;

    if (!strcmp(key, drm_pdev)) {
      if (strcmp(val, gpu_info->base.pdev)) {
        return false;
      }
    } else if (!strcmp(key, drm_client_id)) {
      char *endptr;
      cid = strtoul(val, &endptr, 10);
      if (*endptr)
        continue;
      client_id_set = true;
    } else {
      if (!strcmp(key, xe_drm_intel_vram)) {
        unsigned long mem_int;
        char *endptr;

        mem_int = strtoul(val, &endptr, 10);
        if (endptr == val || (strcmp(endptr, " kB") && strcmp(endptr, " KiB")))
          continue;

        if GPUINFO_PROCESS_FIELD_VALID (process_info, gpu_memory_usage)
          SET_GPUINFO_PROCESS(process_info, gpu_memory_usage, process_info->gpu_memory_usage + (mem_int * 1024));
        else
          SET_GPUINFO_PROCESS(process_info, gpu_memory_usage, mem_int * 1024);
      } else {
        unsigned long cycles;
        char *endptr;

        for (unsigned i = 0; i < ARRAY_SIZE(gpu_cycles.array); i++) {
          if (!strcmp(key, cycles_keys[i])) {
            cycles = strtoull(val, &endptr, 10);
            gpu_cycles.array[i] = cycles;
          }
        }

        for (unsigned i = 0; i < ARRAY_SIZE(total_cycles_keys); i++) {
          if (!strcmp(key, total_cycles_keys[i])) {
            cycles = strtoull(val, &endptr, 10);
            total_cycles.array[i] = cycles;
          }
        }
      }
    }
  }

  {
    uint64_t cycles_sum = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(gpu_cycles.array); i++) {
      cycles_sum += gpu_cycles.array[i];
    }
    SET_GPUINFO_PROCESS(process_info, gpu_cycles, cycles_sum);
  }

  if (!client_id_set)
    return false;

  process_info->type = gpu_process_unknown;
  if (gpu_cycles.rcs != 0)
    process_info->type |= gpu_process_graphical;
  if (gpu_cycles.ccs != 0)
    process_info->type |= gpu_process_compute;

  struct intel_process_info_cache *cache_entry;
  struct unique_cache_id ucid = {.client_id = cid, .pid = process_info->pid, .pdev = gpu_info->base.pdev};
  HASH_FIND_CLIENT(gpu_info->last_update_process_cache, &ucid, cache_entry);
  if (cache_entry) {
    HASH_DEL(gpu_info->last_update_process_cache, cache_entry);

    {
      uint64_t cycles_delta = gpu_cycles.rcs - cache_entry->gpu_cycles.rcs;
      uint64_t total_cycles_delta = total_cycles.rcs - cache_entry->total_cycles.rcs;
      if (total_cycles_delta > 0)
        SET_GPUINFO_PROCESS(process_info, gpu_usage, cycles_delta * 100 / total_cycles_delta);
      else
        SET_GPUINFO_PROCESS(process_info, gpu_usage, 0);
    }
    {
      uint64_t cycles_delta = gpu_cycles.ccs - cache_entry->gpu_cycles.ccs;
      uint64_t total_cycles_delta = total_cycles.ccs - cache_entry->total_cycles.ccs;
      if (total_cycles_delta > 0)
        SET_GPUINFO_PROCESS(process_info, gpu_usage, process_info->gpu_usage + cycles_delta * 100 / total_cycles_delta);
    }
    {
      uint64_t cycles_delta = gpu_cycles.vcs - cache_entry->gpu_cycles.vcs;
      uint64_t total_cycles_delta = total_cycles.vcs - cache_entry->total_cycles.vcs;
      if (total_cycles_delta > 0)
        SET_GPUINFO_PROCESS(process_info, decode_usage, cycles_delta * 100 / total_cycles_delta);
    }

  } else {
    cache_entry = calloc(1, sizeof(*cache_entry));
    if (!cache_entry)
      goto parse_fdinfo_exit;
    cache_entry->client_id.client_id = cid;
    cache_entry->client_id.pid = process_info->pid;
    cache_entry->client_id.pdev = gpu_info->base.pdev;
  }

#ifndef NDEBUG
  struct intel_process_info_cache *cache_entry_check;
  HASH_FIND_CLIENT(gpu_info->current_update_process_cache, &cache_entry->client_id, cache_entry_check);
  assert(!cache_entry_check && "We should not be processing a client id twice per update");
#endif

  RESET_ALL(cache_entry->valid);
  SET_INTEL_CACHE(cache_entry, gpu_cycles, gpu_cycles);
  SET_INTEL_CACHE(cache_entry, total_cycles, total_cycles);

  HASH_ADD_CLIENT(gpu_info->current_update_process_cache, cache_entry);

parse_fdinfo_exit:
  return true;
}
