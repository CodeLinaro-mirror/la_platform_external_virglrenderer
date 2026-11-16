/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_device_memory.h"

#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util/anon_file.h"
#include "vn_protocol_renderer_transport.h"

#include "vkr_device_memory_gen.h"
#include "vkr_physical_device.h"

#if defined(HAVE_LINUX_UDMABUF_H) && defined(HAVE_MEMFD_CREATE)
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

static VkResult
vkr_udmabuf_get_fd_info_from_allocation_info(struct vkr_physical_device *physical_dev,
                                             const VkMemoryAllocateInfo *alloc_info,
                                             int *out_udmabuf_fd,
                                             VkImportMemoryFdInfoKHR *out_fd_info)
{
   int memfd = -1;
   int udmabuf_fd = -1;
   int fd = -1;

   memfd = memfd_create("vkr-udmabuf", MFD_CLOEXEC | MFD_ALLOW_SEALING);
   if (memfd < 0) {
      vkr_log("memfd_create failed (%s)", strerror(errno));
      goto fail;
   }

   const size_t size = align(alloc_info->allocationSize, getpagesize());
   int ret = ftruncate(memfd, size);
   if (ret) {
      vkr_log("ftruncate failed (%s)", strerror(errno));
      goto fail;
   }

   ret = fcntl(memfd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW);
   if (ret) {
      vkr_log("fcntl F_ADD_SEALS failed (%s)", strerror(errno));
      goto fail;
   }

   const struct udmabuf_create create = {
      .memfd = memfd,
      .flags = UDMABUF_FLAGS_CLOEXEC,
      .size = size,
   };
   udmabuf_fd = ioctl(physical_dev->udmabuf_dev_fd, UDMABUF_CREATE, &create);
   if (udmabuf_fd < 0) {
      vkr_log("ioctl UDMABUF_CREATE failed (%s)", strerror(errno));
      goto fail;
   }

   fd = os_dupfd_cloexec(udmabuf_fd);
   if (fd < 0) {
      vkr_log("os_dupfd_cloexec failed (%s)", strerror(errno));
      goto fail;
   }

   close(memfd);

   *out_udmabuf_fd = udmabuf_fd;
   *out_fd_info = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = alloc_info->pNext,
      .fd = fd,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };

   return VK_SUCCESS;

fail:
   if (udmabuf_fd >= 0)
      close(udmabuf_fd);
   if (memfd >= 0)
      close(memfd);
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#else  /* HAVE_LINUX_UDMABUF_H && HAVE_MEMFD_CREATE */

static inline VkResult
vkr_udmabuf_get_fd_info_from_allocation_info(
   UNUSED struct vkr_physical_device *physical_dev,
   UNUSED const VkMemoryAllocateInfo *alloc_info,
   UNUSED int *out_udmabuf_fd,
   UNUSED VkImportMemoryFdInfoKHR *out_fd_info)
{
   vkr_log("udmabuf_allocation is not enabled");
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#endif /* HAVE_LINUX_UDMABUF_H && HAVE_MEMFD_CREATE */

#ifdef ENABLE_GBM_ALLOCATION
#include <gbm.h>

#define GBM_BO_USE_SW_READ_RARELY (1 << 10)
#define GBM_BO_USE_SW_WRITE_RARELY (1 << 12)

static inline int
vkr_gbm_bo_get_fd(void *gbm_bo)
{
   assert(gbm_bo);

   /* gbm_bo_get_fd returns negative error code on failure */
   return gbm_bo_get_fd(gbm_bo);
}

static inline void
vkr_gbm_bo_destroy(void *gbm_bo)
{
   gbm_bo_destroy(gbm_bo);
}

static VkResult
vkr_gbm_get_fd_info_from_allocation_info(struct vkr_physical_device *physical_dev,
                                         const VkMemoryAllocateInfo *alloc_info,
                                         void **out_gbm_bo,
                                         VkImportMemoryFdInfoKHR *out_fd_info)
{
   const uint32_t flags =
      GBM_BO_USE_LINEAR | GBM_BO_USE_SW_READ_RARELY | GBM_BO_USE_SW_WRITE_RARELY;
   struct gbm_bo *gbm_bo;
   int fd = -1;

   assert(physical_dev->gbm_device);

   /*
    * Reject here for simplicity. Letting VkPhysicalDeviceVulkan11Properties return
    * min(maxMemoryAllocationSize, UINT32_MAX) will affect unmappable scenarios.
    */
   if (alloc_info->allocationSize > UINT32_MAX)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* Page alignment is used on all implementations we support. */
   const uint32_t alloc_size = align(alloc_info->allocationSize, getpagesize());
#ifdef MINIGBM
   const uint32_t format = GBM_FORMAT_R8;
   const uint32_t width = alloc_size;
   const uint32_t height = 1;
#else
   /* Mesa gbm has texture size limitations, so we can't rely on R8 here. Instead, we
    * allocate a large enough linear rgba8 buffer.
    */
   const uint32_t format = GBM_FORMAT_ABGR8888;
   const uint8_t pixel_bytes = 4;
   const uint32_t width =
      (uint32_t)ceil(sqrt((alloc_size + pixel_bytes - 1) / pixel_bytes));
   const uint32_t height = width;
#endif /* MINIGBM */

   gbm_bo = gbm_bo_create(physical_dev->gbm_device, width, height, format, flags);
   if (!gbm_bo)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   fd = vkr_gbm_bo_get_fd(gbm_bo);
   if (fd < 0) {
      vkr_gbm_bo_destroy(gbm_bo);
      return fd == -EMFILE ? VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   *out_gbm_bo = (void *)gbm_bo;
   *out_fd_info = (VkImportMemoryFdInfoKHR){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = alloc_info->pNext,
      .fd = fd,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   return VK_SUCCESS;
}

#else

static inline int
vkr_gbm_bo_get_fd(ASSERTED void *gbm_bo)
{
   vkr_log("minigbm_allocation is not enabled");
   assert(!gbm_bo);
   return -1;
}

static inline void
vkr_gbm_bo_destroy(ASSERTED void *gbm_bo)
{
   vkr_log("minigbm_allocation is not enabled");
   assert(!gbm_bo);
}

static inline VkResult
vkr_gbm_get_fd_info_from_allocation_info(UNUSED struct vkr_physical_device *physical_dev,
                                         UNUSED const VkMemoryAllocateInfo *alloc_info,
                                         UNUSED void **out_gbm_bo,
                                         UNUSED VkImportMemoryFdInfoKHR *out_fd_info)
{
   vkr_log("minigbm_allocation is not enabled");
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

#endif /* ENABLE_GBM_ALLOCATION */

/* POSIX shm imported as host memory, backing memory the guest can map */
struct vkr_shm {
   int fd;
   void *ptr;
   size_t size;
};

/* Map the whole of fd at an address aligned to alignment, which may exceed
 * what mmap guarantees: reserve a window large enough to hold an aligned
 * range, map the fd over that range and release the slack around it.
 */
static void *
vkr_shm_map_aligned(int fd, size_t size, size_t alignment)
{
   const size_t page_size = getpagesize();
   if (alignment <= page_size)
      return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

   const size_t window_size = size + alignment;
   uint8_t *window =
      mmap(NULL, window_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (window == MAP_FAILED)
      return MAP_FAILED;

   uint8_t *ptr = (uint8_t *)align64((uintptr_t)window, alignment);
   if (mmap(ptr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) ==
       MAP_FAILED) {
      munmap(window, window_size);
      return MAP_FAILED;
   }

   if (ptr > window)
      munmap(window, ptr - window);
   if (ptr + size < window + window_size)
      munmap(ptr + size, window + window_size - (ptr + size));

   return ptr;
}

/* Create a shm for host memory import. With a valid fd, the existing shm behind it is
 * mapped again instead, and the fd remains owned by the caller.
 */
static struct vkr_shm *
vkr_shm_create(int fd, uint64_t size, uint64_t alignment)
{
   alignment = MAX2(alignment, (uint64_t)getpagesize());
   const size_t aligned_size = align64(size, alignment);

   if (fd >= 0) {
      /* the aligned size must be backed by the existing shm as well */
      struct stat st;
      if (fstat(fd, &st) || st.st_size < 0 || (uint64_t)st.st_size < aligned_size)
         return NULL;
   }

   struct vkr_shm *shm = calloc(1, sizeof(*shm));
   if (!shm)
      return NULL;

   shm->fd = fd < 0 ? os_create_anonymous_file(aligned_size, "vkr-host-mem")
                    : os_dupfd_cloexec(fd);
   if (shm->fd < 0) {
      free(shm);
      return NULL;
   }

   shm->ptr = vkr_shm_map_aligned(shm->fd, aligned_size, alignment);
   if (shm->ptr == MAP_FAILED) {
      close(shm->fd);
      free(shm);
      return NULL;
   }

   shm->size = aligned_size;
   return shm;
}

static void
vkr_shm_destroy(struct vkr_shm *shm)
{
   if (!shm)
      return;

   munmap(shm->ptr, shm->size);
   close(shm->fd);
   free(shm);
}

static VkResult
vkr_shm_get_host_ptr_info_from_allocation_info(
   struct vkr_device *dev,
   const VkMemoryAllocateInfo *alloc_info,
   struct vkr_shm **out_shm,
   VkImportMemoryHostPointerInfoEXT *out_host_ptr_info)
{
   const struct vkr_physical_device *physical_dev = dev->physical_device;
   const VkDeviceSize alignment =
      physical_dev->host_ptr_props.minImportedHostPointerAlignment;
   struct vkr_shm *shm = vkr_shm_create(-1, alloc_info->allocationSize, alignment);
   if (!shm)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   /* Drivers tend to only report cached memory types for host pointer import, so
    * that they don't have to reason about the cache mode associated with the
    * imported pages. Since none of the hw drivers assigns different VA space for
    * coherent vs cached memory types, the import can be safely routed to the
    * non-cached memory types as well:
    * - for VM setup, coherency is maintained by the hypervisor, according to the
    *   cache mode picked when setting up the nested pages
    * - for vtest setup, it's at best effort since cache management is not wired
    *   over the vtest protocol
    *
    * So the reported memory types are only checked for debugging purposes.
    */
   if (VKR_DEBUG(HOST_PTR)) {
      VkMemoryHostPointerPropertiesEXT host_props = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
      };
      const VkResult result = dev->GetMemoryHostPointerPropertiesEXT(
         dev->base.handle.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
         shm->ptr, &host_props);
      /* alloc_info->memoryTypeIndex is the driver's index, which the memory
       * type spoof may have shifted from the one the guest asked for
       */
      if (result != VK_SUCCESS ||
          !(host_props.memoryTypeBits & (1u << alloc_info->memoryTypeIndex))) {
         vkr_log("host memory import is unsupported for memory type %u "
                 "(vk ret %d, memoryTypeBits 0x%x)",
                 alloc_info->memoryTypeIndex, result, host_props.memoryTypeBits);
      }
   }

   *out_shm = shm;
   *out_host_ptr_info = (VkImportMemoryHostPointerInfoEXT){
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
      .pNext = alloc_info->pNext,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
      .pHostPointer = shm->ptr,
   };

   return VK_SUCCESS;
}

struct vkr_blob_mem_info {
   VkImportMemoryFdInfoKHR fd;
   void *gbm_bo;
   int udmabuf_fd;

   VkImportMemoryHostPointerInfoEXT host_ptr;
   struct vkr_shm *shm;

   VkExportMemoryAllocateInfo export;

   uint32_t valid_fd_types;
};

static bool
vkr_fix_import_info(struct vkr_context *ctx,
                    const struct vkr_physical_device *physical_dev,
                    VkMemoryAllocateInfo *alloc_info,
                    void *prev_of_res_info,
                    struct vkr_blob_mem_info *local_info)
{
   const VkImportMemoryResourceInfoMESA *res_info = vkr_pnext_get_next(prev_of_res_info);
   struct vkr_resource *res = vkr_context_get_resource(ctx, res_info->resourceId);
   if (!res) {
      vkr_log("failed to import resource: invalid res_id %u", res_info->resourceId);
      vkr_context_set_fatal(ctx);
      return false;
   }

   if (!res->fd_valid)
      return false;

   switch (res->fd_type) {
   case VIRGL_RESOURCE_FD_DMABUF: {
      if (!physical_dev->EXT_external_memory_dma_buf)
         return false;

      int fd = os_dupfd_cloexec(res->u.fd);
      if (fd < 0)
         return false;

      local_info->fd = (VkImportMemoryFdInfoKHR){
         .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
         .pNext = res_info->pNext,
         .fd = fd,
         .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
      vkr_pnext_set_next(prev_of_res_info, &local_info->fd);
      return true;
   }
   case VIRGL_RESOURCE_FD_SHM: {
      if (!physical_dev->EXT_external_memory_host)
         return false;

      const VkDeviceSize alignment =
         physical_dev->host_ptr_props.minImportedHostPointerAlignment;
      struct vkr_shm *shm = vkr_shm_create(res->u.fd, res->size, alignment);
      if (!shm || alloc_info->allocationSize > shm->size) {
         vkr_shm_destroy(shm);
         return false;
      }

      local_info->shm = shm;
      local_info->host_ptr = (VkImportMemoryHostPointerInfoEXT){
         .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
         .pNext = res_info->pNext,
         .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
         .pHostPointer = shm->ptr,
      };
      vkr_pnext_set_next(prev_of_res_info, &local_info->host_ptr);
      alloc_info->allocationSize = shm->size;
      return true;
   }
   default:
      return false;
   }
}

static VkResult
vkr_fix_export_info(struct vkr_device *dev,
                    VkMemoryAllocateInfo *alloc_info,
                    bool is_import,
                    void *prev_of_export_info,
                    struct vkr_blob_mem_info *local_info)
{
   struct vkr_physical_device *physical_dev = dev->physical_device;
   const VkExportMemoryAllocateInfo *export_info =
      vkr_pnext_get_next(prev_of_export_info);

   /* strip the original export_info from the pNext chain */
   vkr_pnext_set_next(prev_of_export_info, export_info->pNext);

   if (is_import || !export_info->handleTypes)
      return VK_SUCCESS;

   if (physical_dev->gbm_device) {
      assert(physical_dev->EXT_external_memory_dma_buf);
      const VkResult result = vkr_gbm_get_fd_info_from_allocation_info(
         physical_dev, alloc_info, &local_info->gbm_bo, &local_info->fd);
      if (result != VK_SUCCESS)
         return result;

      alloc_info->pNext = &local_info->fd;
      local_info->valid_fd_types = 1 << VIRGL_RESOURCE_FD_DMABUF;
      return VK_SUCCESS;
   }

   if ((export_info->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) &&
       physical_dev->is_dma_buf_fd_export_supported) {
      local_info->export = (VkExportMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
         .pNext = alloc_info->pNext,
         .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      };
      alloc_info->pNext = &local_info->export;
      local_info->valid_fd_types = 1 << VIRGL_RESOURCE_FD_DMABUF;
      return VK_SUCCESS;
   }

#ifdef __APPLE__
   if (physical_dev->EXT_external_memory_host) {
      const VkResult result = vkr_shm_get_host_ptr_info_from_allocation_info(
         dev, alloc_info, &local_info->shm, &local_info->host_ptr);
      if (result != VK_SUCCESS)
         return result;

      alloc_info->pNext = &local_info->host_ptr;
      alloc_info->allocationSize = local_info->shm->size;
      local_info->valid_fd_types = 1 << VIRGL_RESOURCE_FD_SHM;
      return VK_SUCCESS;
   }
#endif

   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

static VkResult
vkr_fix_mappable_info(struct vkr_device *dev,
                      VkMemoryAllocateInfo *alloc_info,
                      struct vkr_blob_mem_info *local_info)
{
   /* XXX Force external memory export or import until a new extension that supports
    * direct export from host visible memory.
    *
    * Most VkImage and VkBuffer are non-external while most VkDeviceMemory are external
    * if allocated with a host visible memory type. We still violate the spec by binding
    * external memory to non-external image or buffer, which needs spec changes with a
    * new extension.
    */
   struct vkr_physical_device *physical_dev = dev->physical_device;
   const bool force_gbm_import = !!physical_dev->gbm_device;
   const bool force_udmabuf_import = physical_dev->udmabuf_dev_fd >= 0;
   const bool support_export = physical_dev->is_dma_buf_fd_export_supported ||
                               physical_dev->is_opaque_fd_export_supported;
   const bool use_host_ptr_import =
      physical_dev->EXT_external_memory_host && !support_export;

   if (force_udmabuf_import) {
      /* To be noted, there are 2 limits for udmabuf:
       * - list_limit: udmabuf_create_list->count limit. Default is 1024.
       * - size_limit_mb: Max size of a dmabuf, in megabytes. Default is 64.
       */
      const VkResult result = vkr_udmabuf_get_fd_info_from_allocation_info(
         physical_dev, alloc_info, &local_info->udmabuf_fd, &local_info->fd);
      if (result != VK_SUCCESS)
         return result;

      alloc_info->pNext = &local_info->fd;
      local_info->valid_fd_types = 1 << VIRGL_RESOURCE_FD_DMABUF;
      return VK_SUCCESS;
   }

   if (force_gbm_import) {
      const VkResult result = vkr_gbm_get_fd_info_from_allocation_info(
         physical_dev, alloc_info, &local_info->gbm_bo, &local_info->fd);
      if (result != VK_SUCCESS)
         return result;

      alloc_info->pNext = &local_info->fd;
      local_info->valid_fd_types = 1 << VIRGL_RESOURCE_FD_DMABUF;
      return VK_SUCCESS;
   }

   if (use_host_ptr_import) {
      const VkResult result = vkr_shm_get_host_ptr_info_from_allocation_info(
         dev, alloc_info, &local_info->shm, &local_info->host_ptr);
      if (result != VK_SUCCESS)
         return result;

      alloc_info->pNext = &local_info->host_ptr;
      alloc_info->allocationSize = local_info->shm->size;
      local_info->valid_fd_types = 1 << VIRGL_RESOURCE_FD_SHM;
      return VK_SUCCESS;
   }

   if (support_export) {
      VkExternalMemoryHandleTypeFlagBits handle_type;
      enum virgl_resource_fd_type fd_type;

      if (physical_dev->is_dma_buf_fd_export_supported) {
         handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
         fd_type = VIRGL_RESOURCE_FD_DMABUF;
      } else {
         handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
         fd_type = VIRGL_RESOURCE_FD_OPAQUE;

         /* Guest virtgpu kernel aligns up blob mem size to the page boundary. No
          * matter dma-buf or opaque fd export allocation, the actual allocation
          * in most cases would follow the same padding. For dma-buf, we are able
          * to allocate and validate via lseek below. For opaque fd, Vulkan spec
          * requires to use the same size with allocation time for the mapper,
          * either vkr_allocator or vulkano, to import and populate the mapping.
          * Accessible mapping given by vkMapMemory is normally limited to just the
          * alloc size. When KVM register the mappings to guest pci bar, that
          * involves an invalid chunk towards the end of the page boundary. As a
          * result, any guest side accelerated instructions that rely on the
          * paddings can end up with Illegal instruction error. The most common
          * trigger is via memcpy'ing valid range to the guest side mapped buffer
          * memory, and then, e.g. __memcpy_avx_unaligned_erms  can hit the error.
          */
         alloc_info->allocationSize = align(alloc_info->allocationSize, getpagesize());
      }

      local_info->export = (VkExportMemoryAllocateInfo){
         .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
         .pNext = alloc_info->pNext,
         .handleTypes = handle_type,
      };
      alloc_info->pNext = &local_info->export;
      local_info->valid_fd_types = 1 << fd_type;
      return VK_SUCCESS;
   }

   return VK_SUCCESS;
}

static void
vkr_dispatch_vkAllocateMemory(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkAllocateMemory *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vkr_physical_device *physical_dev = dev->physical_device;

   VkMemoryAllocateInfo *alloc_info = (VkMemoryAllocateInfo *)args->pAllocateInfo;
   const uint32_t mem_type_index = alloc_info->memoryTypeIndex;
   if (unlikely(mem_type_index >= physical_dev->memory_properties.memoryTypeCount)) {
      args->ret = VK_ERROR_UNKNOWN;
      return;
   }

   const uint32_t property_flags =
      physical_dev->memory_properties.memoryTypes[mem_type_index].propertyFlags;
   if (physical_dev->mem_type_spoof && mem_type_index > 0)
      alloc_info->memoryTypeIndex = mem_type_index - 1;

   struct vkr_blob_mem_info blob_info = {
      .fd = { .fd = -1 },
      .udmabuf_fd = -1,
   };
   void *prev_of_res_info = vkr_find_prev_struct(
      alloc_info, VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA);
   if (prev_of_res_info) {
      /* translate VkImportMemoryResourceInfoMESA into import info in place */
      if (!vkr_fix_import_info(ctx, physical_dev, alloc_info, prev_of_res_info,
                               &blob_info)) {
         args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         return;
      }
   }

   const bool is_import = prev_of_res_info != NULL;
   bool might_export_to_share = false;
   void *prev_of_export_info =
      vkr_find_prev_struct(alloc_info, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
   if (prev_of_export_info) {
      const VkExportMemoryAllocateInfo *export_info =
         vkr_pnext_get_next(prev_of_export_info);
      might_export_to_share = !is_import && export_info->handleTypes;

      /* translate driver export info into renderer export info in place */
      args->ret =
         vkr_fix_export_info(dev, alloc_info, is_import, prev_of_export_info, &blob_info);
      if (args->ret != VK_SUCCESS)
         return;
   }

   /* Skip forcing external if a valid VkImportMemoryResourceInfoMESA is provided, since
    * the mapping will be directly set up from the existing virgl resource, or if
    * might_export_to_share has already configured external backing above.
    */
   const bool might_export_to_map =
      !is_import && !might_export_to_share &&
      (property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
   if (might_export_to_map) {
      args->ret = vkr_fix_mappable_info(dev, alloc_info, &blob_info);
      if (args->ret != VK_SUCCESS)
         return;
   }

   struct vkr_device_memory *mem = vkr_device_memory_create_and_add(ctx, args);
   if (!mem) {
      if (blob_info.fd.fd >= 0)
         close(blob_info.fd.fd);
      if (blob_info.gbm_bo)
         vkr_gbm_bo_destroy(blob_info.gbm_bo);
      if (blob_info.udmabuf_fd >= 0)
         close(blob_info.udmabuf_fd);
      vkr_shm_destroy(blob_info.shm);
      return;
   }

   mem->device = dev;
   mem->might_export_to_share = might_export_to_share;
   mem->property_flags = property_flags;
   mem->valid_fd_types = blob_info.valid_fd_types;
   mem->udmabuf_fd = blob_info.udmabuf_fd;
   mem->gbm_bo = blob_info.gbm_bo;
   mem->shm = blob_info.shm;
   mem->allocation_size = alloc_info->allocationSize;
   mem->memory_type_index = alloc_info->memoryTypeIndex;
}

static void
vkr_dispatch_vkFreeMemory(struct vn_dispatch_context *dispatch,
                          struct vn_command_vkFreeMemory *args)
{
   TRACE_FUNC();
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vkr_device_memory *mem = vkr_device_memory_from_handle(args->memory);
   if (!mem)
      return;

   /* The driver may reference the shm pages until the VkDeviceMemory is
    * freed, so free the driver handle before releasing the backing.
    */
   vkr_device_memory_destroy_driver_handle(ctx, args);
   vkr_device_memory_release(mem);
   vkr_device_remove_object(ctx, dev, &mem->base);
}

static void
vkr_dispatch_vkGetDeviceMemoryCommitment(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceMemoryCommitment *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceMemoryCommitment_args_handle(args);
   vk->GetDeviceMemoryCommitment(args->device, args->memory,
                                 args->pCommittedMemoryInBytes);
}

static void
vkr_dispatch_vkGetDeviceMemoryOpaqueCaptureAddress(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceMemoryOpaqueCaptureAddress *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkGetDeviceMemoryOpaqueCaptureAddress_args_handle(args);
   args->ret = vk->GetDeviceMemoryOpaqueCaptureAddress(args->device, args->pInfo);
}

static void
vkr_dispatch_vkGetMemoryResourcePropertiesMESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetMemoryResourcePropertiesMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   struct vkr_resource *res = vkr_context_get_resource(ctx, args->resourceId);
   if (!res) {
      vkr_log("failed to query resource props: invalid res_id %u", args->resourceId);
      vkr_context_set_fatal(ctx);
      return;
   }

   if (!res->fd_valid) {
      args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      return;
   }

   vn_replace_vkGetMemoryResourcePropertiesMESA_args_handle(args);

   switch (res->fd_type) {
   case VIRGL_RESOURCE_FD_DMABUF: {
      VkMemoryFdPropertiesKHR mem_fd_props = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      };
      args->ret = vk->GetMemoryFdPropertiesKHR(
         args->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, res->u.fd,
         &mem_fd_props);
      if (args->ret != VK_SUCCESS)
         return;

      args->pMemoryResourceProperties->memoryTypeBits = mem_fd_props.memoryTypeBits;
      break;
   }
   case VIRGL_RESOURCE_FD_SHM: {
      const struct vkr_physical_device *physical_dev = dev->physical_device;
      if (!physical_dev->EXT_external_memory_host) {
         args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         return;
      }

      const VkDeviceSize host_ptr_alignment =
         physical_dev->host_ptr_props.minImportedHostPointerAlignment;
      struct vkr_shm *shm = vkr_shm_create(res->u.fd, res->size, host_ptr_alignment);
      if (!shm) {
         args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         return;
      }

      VkMemoryHostPointerPropertiesEXT host_props = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
      };
      args->ret = dev->GetMemoryHostPointerPropertiesEXT(
         args->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, shm->ptr,
         &host_props);
      vkr_shm_destroy(shm);
      if (args->ret != VK_SUCCESS)
         return;

      args->pMemoryResourceProperties->memoryTypeBits = host_props.memoryTypeBits;
      break;
   }
   default:
      args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      return;
   }

   vkr_physical_device_mem_type_spoof(dev->physical_device,
                                      &args->pMemoryResourceProperties->memoryTypeBits);

   VkMemoryResourceAllocationSizePropertiesMESA *alloc_size_props =
      vkr_find_struct(args->pMemoryResourceProperties->pNext,
                      VK_STRUCTURE_TYPE_MEMORY_RESOURCE_ALLOCATION_SIZE_PROPERTIES_MESA);
   if (alloc_size_props)
      alloc_size_props->allocationSize = res->size;
}

void
vkr_context_init_device_memory_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkAllocateMemory = vkr_dispatch_vkAllocateMemory;
   dispatch->dispatch_vkFreeMemory = vkr_dispatch_vkFreeMemory;
   dispatch->dispatch_vkMapMemory = NULL;
   dispatch->dispatch_vkUnmapMemory = NULL;
   dispatch->dispatch_vkFlushMappedMemoryRanges = NULL;
   dispatch->dispatch_vkInvalidateMappedMemoryRanges = NULL;
   dispatch->dispatch_vkGetDeviceMemoryCommitment =
      vkr_dispatch_vkGetDeviceMemoryCommitment;
   dispatch->dispatch_vkGetDeviceMemoryOpaqueCaptureAddress =
      vkr_dispatch_vkGetDeviceMemoryOpaqueCaptureAddress;

   dispatch->dispatch_vkGetMemoryResourcePropertiesMESA =
      vkr_dispatch_vkGetMemoryResourcePropertiesMESA;
}

void
vkr_device_memory_release(struct vkr_device_memory *mem)
{
   vkr_shm_destroy(mem->shm);
   if (mem->gbm_bo)
      vkr_gbm_bo_destroy(mem->gbm_bo);
   if (mem->udmabuf_fd >= 0)
      close(mem->udmabuf_fd);
}

bool
vkr_device_memory_export_blob(struct vkr_device_memory *mem,
                              uint64_t blob_size,
                              uint32_t blob_flags,
                              struct virgl_context_blob *out_blob)
{
   TRACE_FUNC();

   /* a memory can only be exported once; we don't want two resources to point
    * to the same storage.
    */
   if (mem->exported) {
      vkr_log("mem has been exported");
      return false;
   }

   uint32_t map_info = VIRGL_RENDERER_MAP_CACHE_NONE;
   if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE) {
      const bool visible = mem->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      const bool coherent = mem->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      const bool cached = mem->property_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
      if (!visible) {
         vkr_log("mem cannot support mappable blob");
         return false;
      }

      /* XXX guessed */
      map_info = (coherent && cached) ? VIRGL_RENDERER_MAP_CACHE_CACHED
                                      : VIRGL_RENDERER_MAP_CACHE_WC;
   }

   if (mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_SHM)) {
      assert(mem->shm);
      if (blob_size > mem->shm->size) {
         vkr_log("mem shm_size %zu < blob_size %" PRIu64, mem->shm->size, blob_size);
         return false;
      }

      int fd = os_dupfd_cloexec(mem->shm->fd);
      if (fd < 0) {
         vkr_log("mem shm fd dup failed (%s)", strerror(errno));
         return false;
      }

      mem->exported = true;
      *out_blob = (struct virgl_context_blob){
         .type = VIRGL_RESOURCE_FD_SHM,
         .u.fd = fd,
         .map_info = map_info,
      };
      return true;
   }

   const bool can_export_dma_buf = mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_DMABUF);
   const bool can_export_opaque = mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_OPAQUE);
   enum virgl_resource_fd_type fd_type;
   VkExternalMemoryHandleTypeFlagBits handle_type;
   struct virgl_resource_vulkan_info vulkan_info;
   if (blob_flags & VIRGL_RENDERER_BLOB_FLAG_USE_CROSS_DEVICE) {
      if (!can_export_dma_buf) {
         vkr_log("mem cannot export to dma_buf for cross device blob sharing");
         return false;
      }
      fd_type = VIRGL_RESOURCE_FD_DMABUF;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   } else if (can_export_dma_buf) {
      /* prefer dmabuf for easier mapping? */
      fd_type = VIRGL_RESOURCE_FD_DMABUF;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   } else if (can_export_opaque) {
      /* prefer opaque for performance? */
      fd_type = VIRGL_RESOURCE_FD_OPAQUE;
      handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

      STATIC_ASSERT(sizeof(vulkan_info.device_uuid) == VK_UUID_SIZE);
      STATIC_ASSERT(sizeof(vulkan_info.driver_uuid) == VK_UUID_SIZE);

      const VkPhysicalDeviceIDProperties *id_props =
         &mem->device->physical_device->id_properties;
      memcpy(vulkan_info.device_uuid, id_props->deviceUUID, VK_UUID_SIZE);
      memcpy(vulkan_info.driver_uuid, id_props->driverUUID, VK_UUID_SIZE);

      vulkan_info.allocation_size = mem->allocation_size;
      vulkan_info.memory_type_index = mem->memory_type_index;
   } else {
      vkr_log("mem is not exportable");
      return false;
   }

   int fd;
   if (mem->udmabuf_fd >= 0) {
      fd = os_dupfd_cloexec(mem->udmabuf_fd);
      if (fd < 0) {
         vkr_log("mem udmabuf fd dup failed (%s)", strerror(errno));
         return false;
      }
   } else if (mem->gbm_bo) {
      assert(handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
      assert(can_export_dma_buf && !can_export_opaque);

      fd = vkr_gbm_bo_get_fd(mem->gbm_bo);
      if (fd < 0) {
         vkr_log("mem gbm bo export failed (ret %d)", fd);
         return false;
      }
   } else {
      struct vn_device_proc_table *vk = &mem->device->proc_table;
      const VkMemoryGetFdInfoKHR fd_info = {
         .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
         .memory = mem->base.handle.device_memory,
         .handleType = handle_type,
      };
      VkResult ret = vk->GetMemoryFdKHR(mem->device->base.handle.device, &fd_info, &fd);
      if (ret != VK_SUCCESS) {
         vkr_log("mem fd export failed (vk ret %d)", ret);
         return false;
      }
   }

   if (fd_type == VIRGL_RESOURCE_FD_DMABUF) {
      const off_t dma_buf_size = lseek(fd, 0, SEEK_END);
      if (dma_buf_size < 0 || (uint64_t)dma_buf_size < blob_size) {
         vkr_log("mem dma_buf_size %lld < blob_size %" PRIu64, (long long)dma_buf_size,
                 blob_size);
         close(fd);
         return false;
      }
   }

   mem->exported = true;

   *out_blob = (struct virgl_context_blob){
      .type = fd_type,
      .u.fd = fd,
      .map_info = map_info,
      .vulkan_info = vulkan_info,
   };

   return true;
}
