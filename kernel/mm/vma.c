#include "internal.h"

vm_area_t *area_from_list(list_head_t *node) {
    return (vm_area_t *)((uint8_t *)node -
                         __builtin_offsetof(vm_area_t, ordered_node));
}

vm_area_t *area_from_rb(rb_node_t *node) {
    return (vm_area_t *)((uint8_t *)node - __builtin_offsetof(vm_area_t, rb));
}

static void augment_area(vm_area_t *area, const vm_area_t *left,
                         const vm_area_t *right) {
    area->subtree_min_start = area->start;
    area->subtree_max_end = area->end;
    area->subtree_max_gap = 0;
    if (left != 0) {
        area->subtree_min_start = left->subtree_min_start;
        if (left->subtree_max_end > area->subtree_max_end)
            area->subtree_max_end = left->subtree_max_end;
        area->subtree_max_gap = left->subtree_max_gap;
        if (area->start > left->subtree_max_end &&
            area->start - left->subtree_max_end > area->subtree_max_gap) {
            area->subtree_max_gap = area->start - left->subtree_max_end;
        }
    }
    if (right != 0) {
        if (right->subtree_max_end > area->subtree_max_end)
            area->subtree_max_end = right->subtree_max_end;
        if (right->subtree_max_gap > area->subtree_max_gap)
            area->subtree_max_gap = right->subtree_max_gap;
        if (right->subtree_min_start > area->end &&
            right->subtree_min_start - area->end > area->subtree_max_gap) {
            area->subtree_max_gap = right->subtree_min_start - area->end;
        }
    }
}

static vm_area_t *build_sorted_area_tree(list_head_t **item, size_t count,
                                         unsigned depth, unsigned red_depth) {
    if (count == 0U) return 0;
    size_t left_count = count / 2U;
    vm_area_t *left = build_sorted_area_tree(item, left_count,
                                             depth + 1U, red_depth);
    vm_area_t *area = area_from_list(*item);
    *item = (*item)->next;
    area->rb.parent_color = depth == red_depth ? RB_TREE_RED : RB_TREE_BLACK;
    vm_area_t *right = build_sorted_area_tree(item, count - left_count - 1U,
                                             depth + 1U, red_depth);
    area->rb.left = left != 0 ? &left->rb : 0;
    area->rb.right = right != 0 ? &right->rb : 0;
    rb_tree_set_parent(area->rb.left, &area->rb);
    rb_tree_set_parent(area->rb.right, &area->rb);
    /* 子节点摘要均已完成；在同一次自底向上遍历中发布当前节点摘要，
     * 不再对构建完成的树进行第二次遍历。 */
    augment_area(area, left, right);
    return area;
}

/* 调用方持有 map_lock：链表已经排序。以线性时间构建，不重复搜索或旋转；
 * 仅将最深的不完整层标为红色，确保每条空叶路径具有相同黑高。 */
void rebuild_area_tree(vm_space_t *space) {
    size_t count = 0U;
    for (list_head_t *item = space->area_list.next;
         item != &space->area_list; item = item->next) ++count;
    unsigned red_depth = 0U;
    for (size_t nodes = count; nodes != 0U; nodes = (nodes - 1U) / 2U)
        ++red_depth;
    list_head_t *item = space->area_list.next;
    vm_area_t *root = build_sorted_area_tree(&item, count, 0U, red_depth);
    space->areas.root = root != 0 ? &root->rb : 0;
}

static void augment_area_node(rb_node_t *node) {
    augment_area(area_from_rb(node),
                 node->left != 0 ? area_from_rb(node->left) : 0,
                 node->right != 0 ? area_from_rb(node->right) : 0);
}

static void augment_area_rotation(rb_node_t *old_root, rb_node_t *new_root) {
    /* 先重新计算降级节点，因为提升后的节点依赖它。
     * 旋转后外层子树覆盖的区间保持不变。 */
    augment_area_node(old_root);
    augment_area_node(new_root);
}

static void update_area_path(rb_node_t *node, bool stop_unchanged) {
    /* 起点可能是摘要仍有效、但刚挂到新父节点的叶子；至少重算其父节点。
     * 更高层只依赖子树摘要，摘要未变时无需继续传播。 */
    augment_area_node(node);
    for (node = rb_tree_parent(node); node != 0; node = rb_tree_parent(node)) {
        vm_area_t *area = area_from_rb(node);
        uint64_t minimum = area->subtree_min_start;
        uint64_t maximum = area->subtree_max_end;
        uint64_t gap = area->subtree_max_gap;
        augment_area_node(node);
        if (stop_unchanged && minimum == area->subtree_min_start &&
            maximum == area->subtree_max_end && gap == area->subtree_max_gap)
            break;
    }
}

/* 调整结束边界不改变树的排序；调用方保证与相邻区域不重叠。 */
void update_area_tree(vm_area_t *area) {
    update_area_path(&area->rb, true);
}

/* 调用方持有 map_lock；新范围已完成重叠检查。 */
void insert_area_tree(vm_space_t *space, vm_area_t *area) {
    rb_node_t **link = &space->areas.root;
    rb_node_t *parent = 0;
    while (*link != 0) {
        parent = *link;
        link = area->start < area_from_rb(parent)->start ?
               &parent->left : &parent->right;
    }
    rb_tree_node_init(&area->rb);
    rb_tree_set_parent(&area->rb, parent);
    *link = &area->rb;
    /* 执行平衡旋转前，先发布有效的子节点摘要。 */
    update_area_tree(area);
    rb_tree_insert_rebalance_augmented(&space->areas, &area->rb,
                                       augment_area_rotation);
}

static void propagate_area_node(rb_node_t *node) {
    /* 删除时后继可能被搬到更高处，旧位置的摘要未变也必须修复新位置。 */
    update_area_path(node, false);
}

void erase_area_tree(vm_space_t *space, vm_area_t *area) {
    rb_tree_erase_augmented(&space->areas, &area->rb, propagate_area_node,
                            augment_area_rotation);
}

vm_area_t *find_area(const vm_space_t *space, uint64_t address) {
    rb_node_t *node = space->areas.root;
    while (node != 0) {
        vm_area_t *area = area_from_rb(node);
        if (address < area->start) node = node->left;
        else if (address >= area->end) node = node->right;
        else return area;
    }
    return 0;
}

/* 调用方持有 map_lock：返回包含目标地址的 VMA；若地址位于空洞，则返回其后的首个 VMA。范围有序且
 * 互不重叠，因此这是树上的下界查找。 */
vm_area_t *find_area_at_or_after(const vm_space_t *space, uint64_t address) {
    vm_area_t *next = 0;
    rb_node_t *node = space->areas.root;
    while (node != 0) {
        vm_area_t *area = area_from_rb(node);
        if (address >= area->end) {
            node = node->right;
        } else if (address < area->start) {
            next = area;
            node = node->left;
        } else {
            return area;
        }
    }
    return next;
}

bool range_size_valid(uint64_t size) {
    return size != 0 && (size & (PAGE_SIZE - 1ULL)) == 0;
}

bool range_valid(uint64_t address, uint64_t size, uint64_t *end) {
    if (!range_size_valid(size) || address < VM_USER_BASE ||
        (address & (PAGE_SIZE - 1ULL)) != 0 || address >= VM_USER_END ||
        size > VM_USER_END - address) return false;
    *end = address + size;
    return true;
}

static bool find_gap_subtree(rb_node_t *node, uint64_t *cursor, uint64_t size) {
    if (node == 0) return false;
    vm_area_t *area = area_from_rb(node);
    if (area->subtree_max_end <= *cursor) return false;
    if (area->subtree_min_start >= *cursor &&
        size <= area->subtree_min_start - *cursor) return true;
    if (area->subtree_max_gap < size) {
        /* 前一个空洞太小，子树内部也没有可用空洞。跳过整个子树，包括其中密集排列的多个 VMA。 */
        *cursor = area->subtree_max_end;
        return false;
    }
    if (find_gap_subtree(node->left, cursor, size)) return true;
    if (area->start >= *cursor && size <= area->start - *cursor) return true;
    if (*cursor < area->end) *cursor = area->end;
    return find_gap_subtree(node->right, cursor, size);
}

uint64_t find_gap(const vm_space_t *space, uint64_t hint, uint64_t size) {
    uint64_t cursor = hint < VM_USER_BASE ? VM_USER_BASE : hint;
    cursor = (cursor + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (cursor >= VM_USER_END || size > VM_USER_END - cursor) cursor = VM_USER_BASE;
    if (find_gap_subtree(space->areas.root, &cursor, size)) return cursor;
    return size <= VM_USER_END - cursor ? cursor : 0;
}

/* 调用方持有 map_lock，只扫描修改区间及两侧邻居。摘除的节点交给调用方锁外释放。 */
void vm_area_merge_range(vm_space_t *space, vm_area_t *first,
                          list_head_t *range_end, list_head_t *retired) {
    list_head_t *head = &space->area_list;
    /* 右邻居可能被合并，使用它之后的稳定节点作为终点。 */
    list_head_t *stop = range_end != head ? range_end->next : head;
    vm_area_t *left = first;
    if (left->ordered_node.prev != head)
        left = area_from_list(left->ordered_node.prev);
    while (left->ordered_node.next != stop && left->ordered_node.next != head) {
        vm_area_t *right = area_from_list(left->ordered_node.next);
        uint64_t length = left->end - left->start;
        if (left->end == right->start && left->prot == right->prot &&
            left->flags == right->flags && left->object == right->object &&
            left->private_object == right->private_object &&
            right->object_offset >= left->object_offset &&
            right->object_offset - left->object_offset == length) {
            erase_area_tree(space, right);
            left->end = right->end;
            update_area_tree(left);
            list_del(&right->ordered_node);
            list_add_tail(retired, &right->ordered_node);
        } else {
            left = right;
        }
    }
}

vm_area_t *area_allocate(void) {
    vm_area_t *area = (vm_area_t *)kzalloc(sizeof(vm_area_t), 0);
    if (area != 0) list_init(&area->ordered_node);
    return area;
}

/* 准备内部尾段，但不修改源区域、列表或索引。调用方持有 map_lock，并负责清理/发布返回的 VMA。 */
vm_area_t *vm_area_copy_tail(const vm_area_t *area, uint64_t start) {
    vm_area_t *right = area_allocate();
    if (right == 0) return 0;
    right->start = start;
    right->end = area->end;
    right->object_offset = area->object_offset + (start - area->start);
    right->prot = area->prot;
    right->flags = area->flags;
    right->object = area->object;
    vm_object_get(right->object);
    right->private_object = area->private_object;
    vm_object_get(right->private_object);
    return right;
}

vm_area_t *vm_area_split(vm_area_t *area, uint64_t split) {
    if (area == 0 || split <= area->start || split >= area->end) return area;
    vm_area_t *right = vm_area_copy_tail(area, split);
    if (right == 0) return 0;
    area->end = split;
    list_add_before(area->ordered_node.next, &right->ordered_node);
    return right;
}
