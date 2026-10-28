#include "internal.h"

kstatus_t vm_pin_user_page(vm_space_t *space, vaddr_t address, bool writable,
                           page_t **out) {
    if (out == 0) return K_EINVAL;
    *out = 0;
    if (space == 0 || address < VM_USER_BASE || address >= VM_USER_END ||
        (address & (PAGE_SIZE - 1ULL)) != 0U) return K_EINVAL;
    vm_fault_info_t fault = {
        .address = address,
        .access = writable ? VM_PROT_WRITE : VM_PROT_READ,
    };
    /* 地址转换与后备存储引用获取必须和 unmap/mprotect/discard 共用 VMA 保护锁。
     * 绝不固定未拥有的物理页。 */
    /* 缺失的 COW 页面先以只读方式实例化，再由写缺页创建私有可写副本。
     * 两次状态转换后都要重新验证。 */
    const unsigned attempts = 3U;
    for (unsigned attempt = 0U; attempt < attempts; ++attempt) {
        kstatus_t status;
        bool needs_fault = false;
        map_lock(space);
        vm_area_t *area = find_area(space, address);
        if (area == 0 || area->object == 0 ||
            (area->prot & fault.access) != fault.access) {
            status = K_EACCES;
        } else if (area->object->type != VM_OBJECT_ANON &&
                   area->object->type != VM_OBJECT_SHARED) {
            status = K_ENOSYS;
        } else {
            paddr_t physical;
            uint64_t flags = 0U;
            status = x86_translate_page(space->root_table, address, &physical,
                                        &flags);
            needs_fault = status == K_ENOENT;
            if (status == K_OK && writable &&
                !x86_page_entry_writable(flags)) {
                status = K_EACCES;
                needs_fault = true;
            }
            if (status == K_OK) {
                page_t *page = phys_to_page(physical);
                uint64_t index =
                    (area->object_offset + address - area->start) >> PAGE_SHIFT;
                if (page == 0 || !anon_page_pin(area->object, index, page))
                    status = K_EIO;
                else
                    *out = page;
            }
        }
        map_unlock(space);
        if (!needs_fault || attempt + 1U == attempts) return status;
        /* 已驻留的可写页面无需缺页/invlpg 事务。对于缺失页/COW 页，在保护区外解析后，重新验证整个
         * VMA/PTE/后备关系。并发取消映射的页面绝不会被固定。 */
        status = vm_handle_fault(space, &fault);
        if (status != K_OK) return status;
    }
    return K_EIO;
}

void vm_unpin_user_page(page_t *page) {
    /* 最后一个未释放的固定引用可能比 VMA/对象引用存续更久。 */
    anon_page_unpin(page);
}

/* REFACTOR_P6_SHARED_OBJECT_OWNER: shared-object creation and direct access. */

kstatus_t vm_object_create_shared(size_t size, vm_object_t **out) {
    if (out == 0 || size == 0 || (size & (PAGE_SIZE - 1ULL)) != 0) {
        return K_EINVAL;
    }
    return vm_object_create_anon_store(size, VM_OBJECT_SHARED, out);
}

kstatus_t vm_object_shared_page_direct(vm_object_t *object, uint64_t offset,
                                       bool create, uint8_t **out) {
    if (object == 0 || out == 0 || object->type != VM_OBJECT_SHARED ||
        offset >= object->size) {
        return K_EINVAL;
    }

    uint64_t index = (offset & ~(uint64_t)(PAGE_SIZE - 1U)) >> PAGE_SHIFT;
    page_t *page = 0;
    kstatus_t status = anon_page_get(object, index, create, false, &page);
    if (status != K_OK) return status;
    void *direct = phys_to_direct(page_to_phys(page));
    if (direct == 0) return K_EIO;
    *out = (uint8_t *)direct;
    return K_OK;
}
