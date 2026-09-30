#include "vm.h"

#include <stddef.h>
#include <stdint.h>

typedef struct vm_range {
    uint64_t start;
    uint64_t end;

    /* free tree 使用；used tree 中维护但不参与查询。 */
    uint64_t subtree_max;

    struct vm_range *left;
    struct vm_range *right;
    struct vm_range *parent;

    /* 仅 free range 进入地址有序双链表。 */
    struct vm_range *prev;
    struct vm_range *next;

    uint64_t attrs;
    uint32_t type;
    bool red;
} vm_range_t;

static vm_range_t *g_free_nodes;

static inline uint64_t range_size(const vm_range_t *node)
{
    return node->end - node->start;
}

static inline uint64_t node_max(const vm_range_t *node)
{
    return node != NULL ? node->subtree_max : 0;
}

static inline bool is_red(const vm_range_t *node)
{
    return node != NULL && node->red;
}

static inline bool is_black(const vm_range_t *node)
{
    return node == NULL || !node->red;
}

static void recalc(vm_range_t *node)
{
    uint64_t maximum = range_size(node);
    uint64_t left = node_max(node->left);
    uint64_t right = node_max(node->right);

    if (left > maximum)
        maximum = left;
    if (right > maximum)
        maximum = right;

    node->subtree_max = maximum;
}

static void update_up(vm_range_t *node)
{
    while (node != NULL) {
        recalc(node);
        node = node->parent;
    }
}

static bool canonical(uint64_t address)
{
    uint64_t top = address >> 48;
    return top == 0 || top == UINT64_C(0xFFFF);
}

static bool power_of_two(uint64_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

static bool valid_type(vm_region_type_t type)
{
    return type >= VM_REGION_GENERIC && type <= VM_REGION_USER;
}

static bool address_to_offset(const vm_space_t *space,
                              uint64_t addr, uint64_t *out)
{
    if (space == NULL || out == NULL || addr < space->base)
        return false;

    uint64_t offset = addr - space->base;
    if (offset >= space->size)
        return false;

    *out = offset;
    return true;
}

static inline uint64_t offset_to_address(const vm_space_t *space,
                                         uint64_t offset)
{
    return space->base + offset;
}

static bool valid_attrs(uint64_t attrs)
{
    uint64_t cache = attrs & VM_ATTR_CACHE_MASK;

    return cache == 0 ||
           cache == VM_ATTR_CACHE_WB ||
           cache == VM_ATTR_CACHE_WC ||
           cache == VM_ATTR_CACHE_UC ||
           cache == VM_ATTR_CACHE_WT ||
           cache == VM_ATTR_CACHE_WP ||
           cache == VM_ATTR_CACHE_UC_MINUS;
}

static bool page_round(uint64_t size, uint64_t *out)
{
    if (size == 0 || size > UINT64_MAX - (VM_PAGE_SIZE - 1))
        return false;

    *out = (size + VM_PAGE_SIZE - 1) & ~(VM_PAGE_SIZE - 1);
    return true;
}

static bool node_refill(pmm_cpu_t *cpu)
{
    pmm_frame_t frame;

    if (!pmm_alloc4k(cpu, &frame))
        return false;

    vm_range_t *nodes = (vm_range_t *)(uintptr_t)frame;
    uint32_t count = (uint32_t)(VM_PAGE_SIZE / sizeof(vm_range_t));

    for (uint32_t i = 0; i < count; ++i) {
        nodes[i].next = g_free_nodes;
        g_free_nodes = &nodes[i];
    }

    return true;
}

static vm_range_t *node_get(pmm_cpu_t *cpu)
{
    if (g_free_nodes == NULL && !node_refill(cpu))
        return NULL;

    vm_range_t *node = g_free_nodes;
    g_free_nodes = node->next;
    return node;
}

static void node_put(vm_range_t *node)
{
    node->next = g_free_nodes;
    g_free_nodes = node;
}

static void rotate_left(vm_range_t **root, vm_range_t *x)
{
    vm_range_t *y = x->right;

    x->right = y->left;
    if (y->left != NULL)
        y->left->parent = x;

    y->parent = x->parent;

    if (x->parent == NULL)
        *root = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;

    y->left = x;
    x->parent = y;

    recalc(x);
    recalc(y);
}

static void rotate_right(vm_range_t **root, vm_range_t *y)
{
    vm_range_t *x = y->left;

    y->left = x->right;
    if (x->right != NULL)
        x->right->parent = y;

    x->parent = y->parent;

    if (y->parent == NULL)
        *root = x;
    else if (y == y->parent->left)
        y->parent->left = x;
    else
        y->parent->right = x;

    x->right = y;
    y->parent = x;

    recalc(y);
    recalc(x);
}

static void insert_fixup(vm_range_t **root, vm_range_t *node)
{
    while (node->parent != NULL && node->parent->red) {
        vm_range_t *parent = node->parent;
        vm_range_t *grand = parent->parent;

        if (parent == grand->left) {
            vm_range_t *uncle = grand->right;

            if (is_red(uncle)) {
                parent->red = false;
                uncle->red = false;
                grand->red = true;
                node = grand;
            } else {
                if (node == parent->right) {
                    node = parent;
                    rotate_left(root, node);
                    parent = node->parent;
                    grand = parent->parent;
                }

                parent->red = false;
                grand->red = true;
                rotate_right(root, grand);
            }
        } else {
            vm_range_t *uncle = grand->left;

            if (is_red(uncle)) {
                parent->red = false;
                uncle->red = false;
                grand->red = true;
                node = grand;
            } else {
                if (node == parent->left) {
                    node = parent;
                    rotate_right(root, node);
                    parent = node->parent;
                    grand = parent->parent;
                }

                parent->red = false;
                grand->red = true;
                rotate_left(root, grand);
            }
        }
    }

    (*root)->red = false;
}

static void tree_insert(vm_range_t **root, vm_range_t *node)
{
    vm_range_t *parent = NULL;
    vm_range_t *cur = *root;

    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;
    node->red = true;
    node->subtree_max = range_size(node);

    while (cur != NULL) {
        parent = cur;
        cur = node->start < cur->start ? cur->left : cur->right;
    }

    node->parent = parent;

    if (parent == NULL)
        *root = node;
    else if (node->start < parent->start)
        parent->left = node;
    else
        parent->right = node;

    update_up(parent);
    insert_fixup(root, node);
}

static vm_range_t *tree_min(vm_range_t *node)
{
    while (node->left != NULL)
        node = node->left;
    return node;
}

static void transplant(vm_range_t **root,
                       vm_range_t *old_node, vm_range_t *new_node)
{
    if (old_node->parent == NULL)
        *root = new_node;
    else if (old_node == old_node->parent->left)
        old_node->parent->left = new_node;
    else
        old_node->parent->right = new_node;

    if (new_node != NULL)
        new_node->parent = old_node->parent;
}

static void delete_fixup(vm_range_t **root,
                         vm_range_t *node, vm_range_t *parent)
{
    while (node != *root && is_black(node)) {
        if (parent == NULL)
            break;

        if (node == parent->left) {
            vm_range_t *sibling = parent->right;

            if (is_red(sibling)) {
                sibling->red = false;
                parent->red = true;
                rotate_left(root, parent);
                sibling = parent->right;
            }

            if (sibling == NULL ||
                (is_black(sibling->left) && is_black(sibling->right))) {
                if (sibling != NULL)
                    sibling->red = true;
                node = parent;
                parent = node->parent;
            } else {
                if (is_black(sibling->right)) {
                    if (sibling->left != NULL)
                        sibling->left->red = false;
                    sibling->red = true;
                    rotate_right(root, sibling);
                    sibling = parent->right;
                }

                sibling->red = parent->red;
                parent->red = false;
                if (sibling->right != NULL)
                    sibling->right->red = false;

                rotate_left(root, parent);
                node = *root;
                parent = NULL;
            }
        } else {
            vm_range_t *sibling = parent->left;

            if (is_red(sibling)) {
                sibling->red = false;
                parent->red = true;
                rotate_right(root, parent);
                sibling = parent->left;
            }

            if (sibling == NULL ||
                (is_black(sibling->left) && is_black(sibling->right))) {
                if (sibling != NULL)
                    sibling->red = true;
                node = parent;
                parent = node->parent;
            } else {
                if (is_black(sibling->left)) {
                    if (sibling->right != NULL)
                        sibling->right->red = false;
                    sibling->red = true;
                    rotate_left(root, sibling);
                    sibling = parent->left;
                }

                sibling->red = parent->red;
                parent->red = false;
                if (sibling->left != NULL)
                    sibling->left->red = false;

                rotate_right(root, parent);
                node = *root;
                parent = NULL;
            }
        }
    }

    if (node != NULL)
        node->red = false;
}

static void tree_delete(vm_range_t **root, vm_range_t *node)
{
    vm_range_t *moved = node;
    vm_range_t *child;
    vm_range_t *child_parent;
    bool moved_red = moved->red;

    if (node->left == NULL) {
        child = node->right;
        child_parent = node->parent;
        transplant(root, node, node->right);
        update_up(child_parent);
    } else if (node->right == NULL) {
        child = node->left;
        child_parent = node->parent;
        transplant(root, node, node->left);
        update_up(child_parent);
    } else {
        moved = tree_min(node->right);
        moved_red = moved->red;
        child = moved->right;

        if (moved->parent == node) {
            child_parent = moved;
            if (child != NULL)
                child->parent = moved;
        } else {
            vm_range_t *old_parent = moved->parent;

            child_parent = old_parent;
            transplant(root, moved, moved->right);

            moved->right = node->right;
            moved->right->parent = moved;

            update_up(old_parent);
        }

        transplant(root, node, moved);

        moved->left = node->left;
        moved->left->parent = moved;
        moved->red = node->red;

        recalc(moved);
        update_up(moved->parent);
    }

    if (!moved_red)
        delete_fixup(root, child, child_parent);
}

static vm_range_t *tree_floor(vm_range_t *root, uint64_t key)
{
    vm_range_t *cur = root;
    vm_range_t *best = NULL;

    while (cur != NULL) {
        if (key < cur->start) {
            cur = cur->left;
        } else {
            best = cur;
            cur = cur->right;
        }
    }

    return best;
}

static vm_range_t *tree_lower_bound(vm_range_t *root, uint64_t key)
{
    vm_range_t *cur = root;
    vm_range_t *best = NULL;

    while (cur != NULL) {
        if (cur->start >= key) {
            best = cur;
            cur = cur->left;
        } else {
            cur = cur->right;
        }
    }

    return best;
}

static void list_insert_before(vm_space_t *space, vm_range_t *next,
                               vm_range_t *node)
{
    if (next == NULL) {
        node->prev = space->free_tail;
        node->next = NULL;

        if (space->free_tail != NULL)
            space->free_tail->next = node;
        else
            space->free_head = node;

        space->free_tail = node;
        return;
    }

    node->next = next;
    node->prev = next->prev;

    if (next->prev != NULL)
        next->prev->next = node;
    else
        space->free_head = node;

    next->prev = node;
}

static void list_insert_after(vm_space_t *space, vm_range_t *prev,
                              vm_range_t *node)
{
    if (prev == NULL) {
        list_insert_before(space, space->free_head, node);
        return;
    }

    list_insert_before(space, prev->next, node);
}

static void list_remove(vm_space_t *space, vm_range_t *node)
{
    if (node->prev != NULL)
        node->prev->next = node->next;
    else
        space->free_head = node->next;

    if (node->next != NULL)
        node->next->prev = node->prev;
    else
        space->free_tail = node->prev;
}

static void remove_free_node(vm_space_t *space, vm_range_t *node)
{
    list_remove(space, node);
    tree_delete((vm_range_t **)&space->free_root, node);
    node_put(node);
}

static vm_range_t *find_fit(const vm_space_t *space,
                            vm_range_t *node, uint64_t size,
                            uint64_t align, uint64_t *out_start)
{
    if (node == NULL || node->subtree_max < size)
        return NULL;

    if (node->left != NULL && node->left->subtree_max >= size) {
        vm_range_t *found =
            find_fit(space, node->left, size, align, out_start);
        if (found != NULL)
            return found;
    }

    uint64_t bytes = range_size(node);
    if (bytes >= size) {
        uint64_t va = offset_to_address(space, node->start);
        uint64_t delta = (UINT64_C(0) - va) & (align - 1);

        if (delta <= bytes - size) {
            *out_start = node->start + delta;
            return node;
        }
    }

    return find_fit(space, node->right, size, align, out_start);
}

static bool normalize_range(const vm_space_t *space,
                            uint64_t addr, uint64_t size,
                            uint64_t *out_start, uint64_t *out_end)
{
    uint64_t bytes;
    uint64_t start;

    if (space == NULL ||
        !page_round(size, &bytes) ||
        (addr & (VM_PAGE_SIZE - 1)) != 0 ||
        !address_to_offset(space, addr, &start) ||
        bytes > space->size - start)
        return false;

    *out_start = start;
    *out_end = start + bytes;
    return true;
}

static bool reserve_from_free(vm_space_t *space, vm_range_t *node,
                              uint64_t start, uint64_t end)
{
    if (start < node->start || end > node->end || start >= end)
        return false;

    if (start == node->start && end == node->end) {
        remove_free_node(space, node);
    } else if (start == node->start) {
        node->start = end;
        update_up(node);
    } else if (end == node->end) {
        node->end = start;
        update_up(node);
    } else {
        vm_range_t *right = node_get(space->cpu);
        if (right == NULL)
            return false;

        right->start = end;
        right->end = node->end;
        right->type = VM_REGION_GENERIC;
        right->attrs = 0;

        node->end = start;
        update_up(node);

        list_insert_after(space, node, right);
        tree_insert((vm_range_t **)&space->free_root, right);
    }

    space->free_bytes -= end - start;
    return true;
}

static void region_init(vm_range_t *node, uint64_t start, uint64_t end,
                        vm_region_type_t type, uint64_t attrs)
{
    node->start = start;
    node->end = end;
    node->type = (uint32_t)type;
    node->attrs = attrs;
    node->prev = NULL;
    node->next = NULL;
}

static bool add_used_region(vm_space_t *space, vm_range_t *region)
{
    vm_range_t *left =
        tree_floor((vm_range_t *)space->used_root, region->start);
    vm_range_t *right =
        tree_lower_bound((vm_range_t *)space->used_root, region->start);

    if ((left != NULL && region->start < left->end) ||
        (right != NULL && region->end > right->start))
        return false;

    tree_insert((vm_range_t **)&space->used_root, region);
    space->used_bytes += region->end - region->start;
    ++space->region_count;
    return true;
}

static bool free_used_region(vm_space_t *space, vm_range_t *region)
{
    uint64_t start = region->start;
    uint64_t end = region->end;

    vm_range_t *right =
        tree_lower_bound((vm_range_t *)space->free_root, start);
    vm_range_t *left =
        right != NULL ? right->prev : (vm_range_t *)space->free_tail;

    if ((left != NULL && start < left->end) ||
        (right != NULL && end > right->start))
        return false;

    tree_delete((vm_range_t **)&space->used_root, region);
    space->used_bytes -= end - start;
    --space->region_count;

    bool merge_left = left != NULL && left->end == start;
    bool merge_right = right != NULL && end == right->start;

    if (merge_left && merge_right) {
        left->end = right->end;
        update_up(left);
        remove_free_node(space, right);
        node_put(region);
    } else if (merge_left) {
        left->end = end;
        update_up(left);
        node_put(region);
    } else if (merge_right) {
        right->start = start;
        update_up(right);
        node_put(region);
    } else {
        region->type = VM_REGION_GENERIC;
        region->attrs = 0;

        list_insert_before(space, right, region);
        tree_insert((vm_range_t **)&space->free_root, region);
    }

    space->free_bytes += end - start;
    return true;
}

bool vm_space_init(vm_space_t *space, pmm_cpu_t *cpu,
                   vaddr_t base, uint64_t size)
{
    if (space == NULL || cpu == NULL ||
        size == 0 ||
        (base & (VM_PAGE_SIZE - 1)) != 0 ||
        (size & (VM_PAGE_SIZE - 1)) != 0 ||
        !canonical(base))
        return false;

    /*
     * 用最后一个可表示地址验证范围，而不是计算 exclusive end。
     * 完整高半区的 exclusive end 是 2^64，uint64_t 无法表示。
     */
    uint64_t last_offset = size - 1;
    if (base > UINT64_MAX - last_offset)
        return false;

    uint64_t last = base + last_offset;
    if (!canonical(last) ||
        ((base >> 47) & 1U) != ((last >> 47) & 1U))
        return false;

    vm_range_t *initial = node_get(cpu);
    if (initial == NULL)
        return false;

    /* tree 中保存相对 base 的 offset。 */
    initial->start = 0;
    initial->end = size;
    initial->type = VM_REGION_GENERIC;
    initial->attrs = 0;
    initial->prev = NULL;
    initial->next = NULL;

    space->base = base;
    space->size = size;
    space->free_bytes = size;
    space->used_bytes = 0;
    space->region_count = 0;
    space->free_root = NULL;
    space->free_head = initial;
    space->free_tail = initial;
    space->used_root = NULL;
    space->cpu = cpu;

    tree_insert((vm_range_t **)&space->free_root, initial);
    return true;
}

bool vm_reserve(vm_space_t *space, vaddr_t addr, uint64_t size,
                vm_region_type_t type, uint64_t attrs)
{
    uint64_t start;
    uint64_t end;

    if (!normalize_range(space, addr, size, &start, &end) ||
        !valid_type(type) || !valid_attrs(attrs))
        return false;

    vm_range_t *free_node =
        tree_floor((vm_range_t *)space->free_root, start);

    if (free_node == NULL ||
        start < free_node->start || end > free_node->end)
        return false;

    vm_range_t *region = node_get(space->cpu);
    if (region == NULL)
        return false;

    region_init(region, start, end, type, attrs);

    if (!reserve_from_free(space, free_node, start, end)) {
        node_put(region);
        return false;
    }

    if (!add_used_region(space, region)) {
        /*
         * free/used 两棵树正常状态下这里不可能失败。
         * 不尝试隐藏结构损坏。
         */
        return false;
    }

    return true;
}

bool vm_alloc(vm_space_t *space, uint64_t size, uint64_t align,
              vm_region_type_t type, uint64_t attrs,
              vaddr_t *out_addr)
{
    uint64_t bytes;

    if (space == NULL || out_addr == NULL ||
        !page_round(size, &bytes) ||
        !valid_type(type) || !valid_attrs(attrs))
        return false;

    if (align < VM_PAGE_SIZE)
        align = VM_PAGE_SIZE;

    if (!power_of_two(align) || (align & (VM_PAGE_SIZE - 1)) != 0)
        return false;

    uint64_t start;
    vm_range_t *free_node =
        find_fit(space, (vm_range_t *)space->free_root,
                 bytes, align, &start);

    if (free_node == NULL)
        return false;

    vm_range_t *region = node_get(space->cpu);
    if (region == NULL)
        return false;

    region_init(region, start, start + bytes, type, attrs);

    if (!reserve_from_free(space, free_node, start, start + bytes)) {
        node_put(region);
        return false;
    }

    if (!add_used_region(space, region))
        return false;

    *out_addr = offset_to_address(space, start);
    return true;
}

bool vm_query(const vm_space_t *space, vaddr_t addr,
              vm_region_info_t *out_info)
{
    uint64_t offset;

    if (space == NULL || out_info == NULL ||
        !address_to_offset(space, addr, &offset))
        return false;

    vm_range_t *region =
        tree_floor((vm_range_t *)space->used_root, offset);

    if (region == NULL || offset >= region->end)
        return false;

    out_info->start = offset_to_address(space, region->start);
    out_info->size = range_size(region);
    out_info->type = (vm_region_type_t)region->type;
    out_info->attrs = region->attrs;
    return true;
}

bool vm_free(vm_space_t *space, vaddr_t addr)
{
    uint64_t offset;

    if (space == NULL ||
        (addr & (VM_PAGE_SIZE - 1)) != 0 ||
        !address_to_offset(space, addr, &offset))
        return false;

    vm_range_t *region =
        tree_floor((vm_range_t *)space->used_root, offset);

    if (region == NULL || region->start != offset)
        return false;

    return free_used_region(space, region);
}
