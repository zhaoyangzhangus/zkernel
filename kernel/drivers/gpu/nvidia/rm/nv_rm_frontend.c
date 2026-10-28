#include <kernel/kmem.h>
#include <kernel/nv_frontend.h>
#include <kernel/init_userspace.h>

#include "include/nv_rm_driver.h"
#include "include/nv_rm_vendor_bridge.h"

struct nv_frontend_file {
    atomic_uint refs;
    void *vendor_file;
    nv_frontend_file_t *control_file;
    bool closed;
};

typedef struct nv_frontend_mapping_cookie {
    nv_frontend_file_t *file;
    void *vendor_cookie;
} nv_frontend_mapping_cookie_t;

static atomic_uint g_nv_frontend_live;

static void nv_frontend_dispose_vendor_mapping(
    const nv_rm_vendor_frontend_mapping_t *mapping)
{
    if (mapping != 0 && mapping->physical_pages_owned &&
        mapping->physical_pages != 0)
        kfree((void *)mapping->physical_pages);
}

kstatus_t nv_frontend_open(bool control, uint32_t minor,
                           nv_frontend_file_t **out)
{
    nv_frontend_file_t *file;
    void *vendor_file = 0;
    kstatus_t status;

    if (out == 0)
        return K_EINVAL;
    *out = 0;

    if (!nv_frontend_driver_ready()) return K_EDEVREMOVED;

    status = nv_rm_vendor_frontend_open(control, minor, &vendor_file);
    if (status != K_OK || vendor_file == 0)
        return status != K_OK ? status : K_EIO;

    file = (nv_frontend_file_t *)kzalloc(sizeof(*file), 0U);
    if (file == 0) {
        nv_rm_vendor_frontend_close(vendor_file);
        return K_ENOMEM;
    }
    atomic_init(&file->refs, 1U);
    file->vendor_file = vendor_file;
    file->control_file = 0;
    file->closed = false;
    atomic_fetch_add_explicit(&g_nv_frontend_live, 1U,
                              memory_order_relaxed);
    *out = file;
    return K_OK;
}

void nv_frontend_file_get(nv_frontend_file_t *file)
{
    if (file != 0)
        atomic_fetch_add_explicit(&file->refs, 1U, memory_order_relaxed);
}

void nv_frontend_file_put(nv_frontend_file_t *file)
{
    if (file == 0 || atomic_fetch_sub_explicit(&file->refs, 1U,
                                                  memory_order_acq_rel) != 1U)
        return;
    nv_frontend_file_t *control_file = file->control_file;
    file->control_file = 0;
    file->closed = true;
    nv_rm_vendor_frontend_close(file->vendor_file);
    file->vendor_file = 0;
    if (control_file != 0)
        nv_frontend_file_put(control_file);
    atomic_fetch_sub_explicit(&g_nv_frontend_live, 1U,
                              memory_order_relaxed);
    kfree(file);
}

void *nv_frontend_file_vendor_private(nv_frontend_file_t *file)
{
    void *private_data = 0;

    if (file == 0 || file->closed || file->vendor_file == 0 ||
        nv_rm_vendor_frontend_abi_private(file->vendor_file,
                                          &private_data) != K_OK)
        return 0;
    return private_data;
}

bool nv_frontend_file_is_control(const nv_frontend_file_t *file)
{
    if (file == 0 || file->closed || file->vendor_file == 0)
        return false;
    return nv_rm_vendor_frontend_is_control(file->vendor_file);
}

kstatus_t nv_frontend_ioctl(nv_frontend_file_t *file, uint32_t request,
                            void *argument, uint32_t argument_size)
{
    if (file == 0 || file->closed || file->vendor_file == 0 ||
        argument == 0 || argument_size == 0U)
        return K_EBADF;
    return nv_rm_vendor_frontend_ioctl(file->vendor_file, request, argument,
                                        argument_size);
}

uint32_t nv_frontend_poll_state(nv_frontend_file_t *file, bool consume_dataless)
{
    if (file == 0 || file->closed || file->vendor_file == 0)
        return NV_FRONTEND_POLL_CLOSED;
    return nv_rm_vendor_frontend_poll_state(file->vendor_file, consume_dataless);
}

void *nv_frontend_file_wait_object(nv_frontend_file_t *file)
{
    if (file == 0 || file->closed || file->vendor_file == 0) return 0;
    return nv_rm_vendor_frontend_wait_object(file->vendor_file);
}

kstatus_t nv_frontend_register_fd(nv_frontend_file_t *file,
                                  nv_frontend_file_t *control_file)
{
    kstatus_t status;

    if (file == 0 || control_file == 0 || file == control_file ||
        file->closed || control_file->closed || file->vendor_file == 0 ||
        control_file->vendor_file == 0 || file->control_file != 0)
        return K_EINVAL;
    status = nv_rm_vendor_frontend_register_fd(file->vendor_file,
                                                control_file->vendor_file);
    if (status != K_OK)
        return status;
    nv_frontend_file_get(control_file);
    file->control_file = control_file;
    return K_OK;
}

kstatus_t nv_frontend_mmap(nv_frontend_file_t *file, uint64_t offset,
                           uint64_t length, uint32_t protection,
                           nv_frontend_mapping_t *mapping)
{
    nv_rm_vendor_frontend_mapping_t vendor_mapping = {0};
    nv_frontend_mapping_cookie_t *cookie;
    kstatus_t status;

    if (file == 0 || file->closed || file->vendor_file == 0 ||
        mapping == 0)
        return K_EBADF;
    *mapping = (nv_frontend_mapping_t){0};

    status = nv_rm_vendor_frontend_mmap(file->vendor_file, offset, length,
                                        protection, &vendor_mapping);
    if (status != K_OK)
        return status;
    if (vendor_mapping.length != length ||
        (vendor_mapping.length & (PAGE_SIZE - 1ULL)) != 0U ||
        vendor_mapping.length == 0U) {
        nv_frontend_dispose_vendor_mapping(&vendor_mapping);
        return K_EINVAL;
    }
    if (vendor_mapping.physical_pages != 0 ||
        vendor_mapping.page_count != 0U) {
        if (vendor_mapping.physical_pages == 0 ||
            vendor_mapping.page_count != vendor_mapping.length / PAGE_SIZE) {
            nv_frontend_dispose_vendor_mapping(&vendor_mapping);
            return K_EINVAL;
        }
    } else if ((vendor_mapping.physical.value & (PAGE_SIZE - 1ULL)) != 0U) {
        nv_frontend_dispose_vendor_mapping(&vendor_mapping);
        return K_EINVAL;
    }

    cookie = (nv_frontend_mapping_cookie_t *)kzalloc(sizeof(*cookie), 0U);
    if (cookie == 0) {
        nv_frontend_dispose_vendor_mapping(&vendor_mapping);
        return K_ENOMEM;
    }
    nv_frontend_file_get(file);
    cookie->file = file;
    cookie->vendor_cookie = vendor_mapping.physical_pages_owned ?
                            (void *)vendor_mapping.physical_pages : 0;
    mapping->physical = vendor_mapping.physical;
    mapping->physical_pages = vendor_mapping.physical_pages;
    mapping->page_count = vendor_mapping.page_count;
    mapping->length = vendor_mapping.length;
    mapping->cache_mode = vendor_mapping.cache_mode;
    mapping->cookie = cookie;
    return K_OK;
}

void nv_frontend_mapping_release(void *opaque_cookie)
{
    nv_frontend_mapping_cookie_t *cookie =
        (nv_frontend_mapping_cookie_t *)opaque_cookie;
    if (cookie == 0)
        return;
    if (cookie->vendor_cookie != 0) {
        kfree(cookie->vendor_cookie);
        cookie->vendor_cookie = 0;
    }
    /* The file reference keeps the RM mapping context and its allocation
     * private object alive until the VM object has been torn down. */
    nv_frontend_file_put(cookie->file);
    kfree(cookie);
}

uint32_t nv_frontend_live_count(void)
{
    return atomic_load_explicit(&g_nv_frontend_live, memory_order_acquire);
}

bool nv_frontend_driver_ready(void)
{
    /* Only the process touching NVIDIA devices waits, never the compositor.
     * The completion has boot-long lifetime, independent of worker cleanup. */
    if (!userspace_wait_device_init()) return false;
    const nv_gpu_device_t *gpu = nv_rm_driver_current();
    return gpu != 0 && gpu->state == NV_GPU_GSP_READY;
}
