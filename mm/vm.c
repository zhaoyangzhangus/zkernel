#include "vm.h"

#include <stddef.h>
#include <stdint.h>

typedef struct vm_range {
    uint64_t start;
    uint64_t end;

    /* 当前 RB 子树中的最大 free range。 */
    uint64_t subtree_max;

    struct vm_range *left;
    struct vm_range *right;
    struct vm_range *parent;

    /* 地址顺序链表，用于 O(1) 取得前后相邻 free range。 */
    struct vm_range *prev;
    struct vm_range *next;

    bool red;
} vm_range_t;

/*
 * range metadata node cache。
 * 当前仍使用 UEFI identity map，所以 PMM 返回的 frame 可以直接作为指针。
 */
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

static bool page_round(uint64_t size, uint64_t *out)
{
    if (size == 0 || size > UINT64_MAX - (VM_PAGE_SIZE - 1))
        return false;

    *out = (size + VM_PAGE_SIZE - 1) & ~(VM_PAGE_SIZE - 1);
    return true;
}

static bool align_up(uint64_t value, uint64_t align, uint64_t *out)
{
    uint64_t mask = align - 1;

    if (value > UINT64_MAX - mask)
        return false;

    *out = (value + mask) & ~mask;
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

static void rotate_left(vm_space_t *space, vm_range_t *x)
{
    vm_range_t *y = x->right;

    x->right = y->left;
    if (y->left != NULL)
        y->left->parent = x;

    y->parent = x->parent;

    if (x->parent == NULL)
        space->root = y;
    else if (x == x->parent->left)
        x->parent->left = y;
    else
        x->parent->right = y;

    y->left = x;
    x->parent = y;

    recalc(x);
    recalc(y);
}

static void rotate_right(vm_space_t *space, vm_range_t *y)
{
    vm_range_t *x = y->left;

    y->left = x->right;
    if (x->right != NULL)
        x->right->parent = y;

    x->parent = y->parent;

    if (y->parent == NULL)
        space->root = x;
    else if (y == y->parent->left)
        y->parent->left = x;
    else
        y->parent->right = x;

    x->right = y;
    y->parent = x;

    recalc(y);
    recalc(x);
}

static void insert_fixup(vm_space_t *space, vm_range_t *node)
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
                    rotate_left(space, node);
                    parent = node->parent;
                    grand = parent->parent;
                }

                parent->red = false;
                grand->red = true;
                rotate_right(space, grand);
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
                    rotate_right(space, node);
                    parent = node->parent;
                    grand = parent->parent;
                }

                parent->red = false;
                grand->red = true;
                rotate_left(space, grand);
            }
        }
    }

    space->root->red = false;
}

static void tree_insert(vm_space_t *space, vm_range_t *node)
{
    vm_range_t *parent = NULL;
    vm_range_t *cur = space->root;

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
        space->root = node;
    else if (node->start < parent->start)
        parent->left = node;
    else
        parent->right = node;

    update_up(parent);
    insert_fixup(space, node);
}

static vm_range_t *tree_min(vm_range_t *node)
{
    while (node->left != NULL)
        node = node->left;
    return node;
}

static void transplant(vm_space_t *space, vm_range_t *old_node,
                       vm_range_t *new_node)
{
    if (old_node->parent == NULL)
        space->root = new_node;
    else if (old_node == old_node->parent->left)
        old_node->parent->left = new_node;
    else
        old_node->parent->right = new_node;

    if (new_node != NULL)
        new_node->parent = old_node->parent;
}

static void delete_fixup(vm_space_t *space, vm_range_t *node,
                         vm_range_t *parent)
{
    while (node != space->root && is_black(node)) {
        if (parent == NULL)
            break;

        if (node == parent->left) {
            vm_range_t *sibling = parent->right;

            if (is_red(sibling)) {
                sibling->red = false;
                parent->red = true;
                rotate_left(space, parent);
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
                    rotate_right(space, sibling);
                    sibling = parent->right;
                }

                sibling->red = parent->red;
                parent->red = false;
                if (sibling->right != NULL)
                    sibling->right->red = false;

                rotate_left(space, parent);
                node = space->root;
                parent = NULL;
            }
        } else {
            vm_range_t *sibling = parent->left;

            if (is_red(sibling)) {
                sibling->red = false;
                parent->red = true;
                rotate_right(space, parent);
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
                    rotate_left(space, sibling);
                    sibling = parent->left;
                }

                sibling->red = parent->red;
                parent->red = false;
                if (sibling->left != NULL)
                    sibling->left->red = false;

                rotate_right(space, parent);
                node = space->root;
                parent = NULL;
            }
        }
    }

    if (node != NULL)
        node->red = false;
}

static void tree_delete(vm_space_t *space, vm_range_t *node)
{
    vm_range_t *moved = node;
    vm_range_t *child;
    vm_range_t *child_parent;
    bool moved_red = moved->red;

    if (node->left == NULL) {
        child = node->right;
        child_parent = node->parent;
        transplant(space, node, node->right);
        update_up(child_parent);
    } else if (node->right == NULL) {
        child = node->left;
        child_parent = node->parent;
        transplant(space, node, node->left);
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
            transplant(space, moved, moved->right);

            moved->right = node->right;
            moved->right->parent = moved;

            update_up(old_parent);
        }

        transplant(space, node, moved);

        moved->left = node->left;
        moved->left->parent = moved;
        moved->red = node->red;

        recalc(moved);
        update_up(moved->parent);
    }

    if (!moved_red)
        delete_fixup(space, child, child_parent);
}

static void list_insert_before(vm_space_t *space, vm_range_t *next,
                               vm_range_t *node)
{
    if (next == NULL) {
        node->prev = space->tail;
        node->next = NULL;

        if (space->tail != NULL)
            space->tail->next = node;
        else
            space->head = node;

        space->tail = node;
        return;
    }

    node->next = next;
    node->prev = next->prev;

    if (next->prev != NULL)
        next->prev->next = node;
    else
        space->head = node;

    next->prev = node;
}

static void list_insert_after(vm_space_t *space, vm_range_t *prev,
                              vm_range_t *node)
{
    if (prev == NULL) {
        list_insert_before(space, space->head, node);
        return;
    }

    list_insert_before(space, prev->next, node);
}

static void list_remove(vm_space_t *space, vm_range_t *node)
{
    if (node->prev != NULL)
        node->prev->next = node->next;
    else
        space->head = node->next;

    if (node->next != NULL)
        node->next->prev = node->prev;
    else
        space->tail = node->prev;
}

static void remove_free_node(vm_space_t *space, vm_range_t *node)
{
    list_remove(space, node);
    tree_delete(space, node);
    node_put(node);
}

static vm_range_t *tree_floor(const vm_space_t *space, uint64_t key)
{
    vm_range_t *cur = space->root;
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

static vm_range_t *tree_lower_bound(const vm_space_t *space, uint64_t key)
{
    vm_range_t *cur = space->root;
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

/*
 * 找地址最低的可用区间。
 * subtree_max < size 的整棵子树可以直接跳过。
 * alignment 只在候选 node 上计算，所以超大对齐时最坏情况仍可能检查
 * 多个候选；普通 4K/2M 对齐下可快速跳过绝大多数不可能满足的子树。
 */
static vm_range_t *find_fit(vm_range_t *node, uint64_t size,
                            uint64_t align, uint64_t *out_start)
{
    if (node == NULL || node->subtree_max < size)
        return NULL;

    if (node->left != NULL && node->left->subtree_max >= size) {
        vm_range_t *found = find_fit(node->left, size, align, out_start);
        if (found != NULL)
            return found;
    }

    uint64_t start;
    if (range_size(node) >= size &&
        align_up(node->start, align, &start) &&
        start >= node->start &&
        start <= UINT64_MAX - size &&
        start + size <= node->end) {
        *out_start = start;
        return node;
    }

    return find_fit(node->right, size, align, out_start);
}

static bool normalize_range(const vm_space_t *space,
                            uint64_t addr, uint64_t size,
                            uint64_t *out_end)
{
    uint64_t bytes;

    if (space == NULL || !page_round(size, &bytes) ||
        (addr & (VM_PAGE_SIZE - 1)) != 0 ||
        addr < space->base ||
        addr > UINT64_MAX - bytes)
        return false;

    uint64_t end = addr + bytes;

    if (end > space->end)
        return false;

    *out_end = end;
    return true;
}

static bool reserve_from_node(vm_space_t *space, vm_range_t *node,
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

        node->end = start;
        update_up(node);

        list_insert_after(space, node, right);
        tree_insert(space, right);
    }

    space->free_bytes -= end - start;
    return true;
}

bool vm_space_init(vm_space_t *space, pmm_cpu_t *cpu,
                   vaddr_t base, uint64_t size)
{
    if (space == NULL || cpu == NULL ||
        size == 0 ||
        (base & (VM_PAGE_SIZE - 1)) != 0 ||
        (size & (VM_PAGE_SIZE - 1)) != 0 ||
        base > UINT64_MAX - size)
        return false;

    uint64_t end = base + size;

    if (!canonical(base) || !canonical(end - 1) ||
        ((base >> 47) & 1U) != (((end - 1) >> 47) & 1U))
        return false;

    vm_range_t *initial = node_get(cpu);
    if (initial == NULL)
        return false;

    initial->start = base;
    initial->end = end;
    initial->prev = NULL;
    initial->next = NULL;

    space->base = base;
    space->end = end;
    space->free_bytes = size;
    space->root = NULL;
    space->head = initial;
    space->tail = initial;
    space->cpu = cpu;

    tree_insert(space, initial);
    return true;
}

bool vm_reserve(vm_space_t *space, vaddr_t addr, uint64_t size)
{
    uint64_t end;

    if (!normalize_range(space, addr, size, &end))
        return false;

    vm_range_t *node = tree_floor(space, addr);

    if (node == NULL || addr < node->start || end > node->end)
        return false;

    return reserve_from_node(space, node, addr, end);
}

bool vm_alloc(vm_space_t *space, uint64_t size, uint64_t align,
              vaddr_t *out_addr)
{
    uint64_t bytes;

    if (space == NULL || out_addr == NULL || !page_round(size, &bytes))
        return false;

    if (align < VM_PAGE_SIZE)
        align = VM_PAGE_SIZE;

    if (!power_of_two(align) || (align & (VM_PAGE_SIZE - 1)) != 0)
        return false;

    uint64_t start;
    vm_range_t *node = find_fit(space->root, bytes, align, &start);

    if (node == NULL)
        return false;

    if (!reserve_from_node(space, node, start, start + bytes))
        return false;

    *out_addr = start;
    return true;
}

bool vm_free(vm_space_t *space, vaddr_t addr, uint64_t size)
{
    uint64_t end;

    if (!normalize_range(space, addr, size, &end))
        return false;

    vm_range_t *right = tree_lower_bound(space, addr);
    vm_range_t *left = right != NULL ? right->prev : space->tail;

    /* 与现有 free range 重叠：double-free 或错误区间。 */
    if ((left != NULL && addr < left->end) ||
        (right != NULL && end > right->start))
        return false;

    bool merge_left = left != NULL && left->end == addr;
    bool merge_right = right != NULL && end == right->start;

    if (merge_left && merge_right) {
        left->end = right->end;
        update_up(left);
        remove_free_node(space, right);
    } else if (merge_left) {
        left->end = end;
        update_up(left);
    } else if (merge_right) {
        /*
         * right 是 lower_bound(addr)，addr 与旧 right->start 之间没有
         * 其他 key，因此把 start 向左扩到 addr 不改变树的全局排序关系。
         */
        right->start = addr;
        update_up(right);
    } else {
        vm_range_t *node = node_get(space->cpu);
        if (node == NULL)
            return false;

        node->start = addr;
        node->end = end;

        list_insert_before(space, right, node);
        tree_insert(space, node);
    }

    space->free_bytes += end - addr;
    return true;
}
