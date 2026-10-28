#pragma once
#pragma once
#include "base.h"

typedef struct rb_node {
    uintptr_t parent_color;
    struct rb_node *left;
    struct rb_node *right;
} rb_node_t;

typedef struct rb_root {
    rb_node_t *root;
} rb_root_t;

enum {
    RB_TREE_RED = 0U,
    RB_TREE_BLACK = 1U,
};

static inline void rb_tree_root_init(rb_root_t *root) {
    root->root = 0;
}

static inline void rb_tree_node_init(rb_node_t *node) {
    node->parent_color = 0U;
    node->left = 0;
    node->right = 0;
}

static inline rb_node_t *rb_tree_parent(const rb_node_t *node) {
    return node == 0 ? 0 :
           (rb_node_t *)(node->parent_color & ~(uintptr_t)3U);
}

static inline unsigned rb_tree_color(const rb_node_t *node) {
    return node == 0 ? RB_TREE_BLACK :
           (unsigned)(node->parent_color & (uintptr_t)1U);
}

static inline void rb_tree_set_parent(rb_node_t *node, rb_node_t *parent) {
    if (node != 0) {
        node->parent_color = (node->parent_color & (uintptr_t)1U) |
                             (uintptr_t)parent;
    }
}

static inline void rb_tree_set_color(rb_node_t *node, unsigned color) {
    if (node != 0) {
        node->parent_color = (node->parent_color & ~(uintptr_t)1U) |
                             (uintptr_t)(color & 1U);
    }
}

static inline void rb_tree_rotate_left(rb_root_t *root, rb_node_t *node) {
    rb_node_t *right = node->right;
    rb_node_t *parent = rb_tree_parent(node);
    node->right = right->left;
    if (right->left != 0) rb_tree_set_parent(right->left, node);
    right->left = node;
    rb_tree_set_parent(node, right);
    rb_tree_set_parent(right, parent);
    if (parent == 0) root->root = right;
    else if (parent->left == node) parent->left = right;
    else parent->right = right;
}

static inline void rb_tree_rotate_right(rb_root_t *root, rb_node_t *node) {
    rb_node_t *left = node->left;
    rb_node_t *parent = rb_tree_parent(node);
    node->left = left->right;
    if (left->right != 0) rb_tree_set_parent(left->right, node);
    left->right = node;
    rb_tree_set_parent(node, left);
    rb_tree_set_parent(left, parent);
    if (parent == 0) root->root = left;
    else if (parent->left == node) parent->left = left;
    else parent->right = left;
}

/* 调用方须先将 node 链接到 parent 下方并标记为红色，再调用此函数。 */
static inline void rb_tree_insert_rebalance_augmented(
    rb_root_t *root, rb_node_t *node,
    void (*rotate)(rb_node_t *old_root, rb_node_t *new_root)) {
    rb_node_t *parent;
    while ((parent = rb_tree_parent(node)) != 0 &&
           rb_tree_color(parent) == RB_TREE_RED) {
        rb_node_t *grand = rb_tree_parent(parent);
        if (parent == grand->left) {
            rb_node_t *uncle = grand->right;
            if (rb_tree_color(uncle) == RB_TREE_RED) {
                rb_tree_set_color(parent, RB_TREE_BLACK);
                rb_tree_set_color(uncle, RB_TREE_BLACK);
                rb_tree_set_color(grand, RB_TREE_RED);
                node = grand;
            } else {
                if (node == parent->right) {
                    node = parent;
                    rb_tree_rotate_left(root, node);
                    if (rotate != 0) rotate(node, rb_tree_parent(node));
                    parent = rb_tree_parent(node);
                    grand = rb_tree_parent(parent);
                }
                rb_tree_set_color(parent, RB_TREE_BLACK);
                rb_tree_set_color(grand, RB_TREE_RED);
                rb_tree_rotate_right(root, grand);
                if (rotate != 0) rotate(grand, rb_tree_parent(grand));
            }
        } else {
            rb_node_t *uncle = grand->left;
            if (rb_tree_color(uncle) == RB_TREE_RED) {
                rb_tree_set_color(parent, RB_TREE_BLACK);
                rb_tree_set_color(uncle, RB_TREE_BLACK);
                rb_tree_set_color(grand, RB_TREE_RED);
                node = grand;
            } else {
                if (node == parent->left) {
                    node = parent;
                    rb_tree_rotate_right(root, node);
                    if (rotate != 0) rotate(node, rb_tree_parent(node));
                    parent = rb_tree_parent(node);
                    grand = rb_tree_parent(parent);
                }
                rb_tree_set_color(parent, RB_TREE_BLACK);
                rb_tree_set_color(grand, RB_TREE_RED);
                rb_tree_rotate_left(root, grand);
                if (rotate != 0) rotate(grand, rb_tree_parent(grand));
            }
        }
    }
    rb_tree_set_color(root->root, RB_TREE_BLACK);
}

static inline void rb_tree_insert_rebalance(rb_root_t *root, rb_node_t *node) {
    rb_tree_insert_rebalance_augmented(root, node, 0);
}

static inline rb_node_t *rb_tree_first(const rb_root_t *root) {
    rb_node_t *node = root->root;
    if (node == 0) return 0;
    while (node->left != 0) node = node->left;
    return node;
}

static inline rb_node_t *rb_tree_next(rb_node_t *node) {
    rb_node_t *parent;
    if (node == 0) return 0;
    if (node->right != 0) {
        node = node->right;
        while (node->left != 0) node = node->left;
        return node;
    }
    while ((parent = rb_tree_parent(node)) != 0 && node == parent->right) {
        node = parent;
    }
    return parent;
}

static inline void rb_tree_transplant(rb_root_t *root, rb_node_t *old_node,
                                       rb_node_t *new_node) {
    rb_node_t *parent = rb_tree_parent(old_node);
    if (parent == 0) root->root = new_node;
    else if (old_node == parent->left) parent->left = new_node;
    else parent->right = new_node;
    rb_tree_set_parent(new_node, parent);
}

static inline void rb_tree_erase_rebalance_augmented(
    rb_root_t *root, rb_node_t *node, rb_node_t *parent,
    void (*rotate)(rb_node_t *old_root, rb_node_t *new_root)) {
    while (node != root->root && rb_tree_color(node) == RB_TREE_BLACK) {
        if (parent == 0) break;
        if (node == parent->left) {
            rb_node_t *sibling = parent->right;
            if (rb_tree_color(sibling) == RB_TREE_RED) {
                rb_tree_set_color(sibling, RB_TREE_BLACK);
                rb_tree_set_color(parent, RB_TREE_RED);
                rb_tree_rotate_left(root, parent);
                if (rotate != 0) rotate(parent, rb_tree_parent(parent));
                sibling = parent->right;
            }
            if (sibling == 0) {
                node = parent;
                parent = rb_tree_parent(parent);
                continue;
            }
            if (rb_tree_color(sibling->left) == RB_TREE_BLACK &&
                rb_tree_color(sibling->right) == RB_TREE_BLACK) {
                rb_tree_set_color(sibling, RB_TREE_RED);
                node = parent;
                parent = rb_tree_parent(parent);
            } else {
                if (rb_tree_color(sibling->right) == RB_TREE_BLACK) {
                    rb_tree_set_color(sibling->left, RB_TREE_BLACK);
                    rb_tree_set_color(sibling, RB_TREE_RED);
                    rb_tree_rotate_right(root, sibling);
                    if (rotate != 0) rotate(sibling, rb_tree_parent(sibling));
                    sibling = parent->right;
                }
                rb_tree_set_color(sibling, rb_tree_color(parent));
                rb_tree_set_color(parent, RB_TREE_BLACK);
                rb_tree_set_color(sibling->right, RB_TREE_BLACK);
                rb_tree_rotate_left(root, parent);
                if (rotate != 0) rotate(parent, rb_tree_parent(parent));
                node = root->root;
                parent = 0;
            }
        } else {
            rb_node_t *sibling = parent->left;
            if (rb_tree_color(sibling) == RB_TREE_RED) {
                rb_tree_set_color(sibling, RB_TREE_BLACK);
                rb_tree_set_color(parent, RB_TREE_RED);
                rb_tree_rotate_right(root, parent);
                if (rotate != 0) rotate(parent, rb_tree_parent(parent));
                sibling = parent->left;
            }
            if (sibling == 0) {
                node = parent;
                parent = rb_tree_parent(parent);
                continue;
            }
            if (rb_tree_color(sibling->left) == RB_TREE_BLACK &&
                rb_tree_color(sibling->right) == RB_TREE_BLACK) {
                rb_tree_set_color(sibling, RB_TREE_RED);
                node = parent;
                parent = rb_tree_parent(parent);
            } else {
                if (rb_tree_color(sibling->left) == RB_TREE_BLACK) {
                    rb_tree_set_color(sibling->right, RB_TREE_BLACK);
                    rb_tree_set_color(sibling, RB_TREE_RED);
                    rb_tree_rotate_left(root, sibling);
                    if (rotate != 0) rotate(sibling, rb_tree_parent(sibling));
                    sibling = parent->left;
                }
                rb_tree_set_color(sibling, rb_tree_color(parent));
                rb_tree_set_color(parent, RB_TREE_BLACK);
                rb_tree_set_color(sibling->left, RB_TREE_BLACK);
                rb_tree_rotate_right(root, parent);
                if (rotate != 0) rotate(parent, rb_tree_parent(parent));
                node = root->root;
                parent = 0;
            }
        }
    }
    rb_tree_set_color(node, RB_TREE_BLACK);
}

static inline void rb_tree_erase_rebalance(rb_root_t *root, rb_node_t *node,
                                           rb_node_t *parent) {
    rb_tree_erase_rebalance_augmented(root, node, parent, 0);
}

static inline void rb_tree_erase_augmented(
    rb_root_t *root, rb_node_t *node, void (*propagate)(rb_node_t *node),
    void (*rotate)(rb_node_t *old_root, rb_node_t *new_root)) {
    rb_node_t *moved = node;
    unsigned moved_color = rb_tree_color(moved);
    rb_node_t *child;
    rb_node_t *child_parent;

    if (node->left == 0) {
        child = node->right;
        child_parent = rb_tree_parent(node);
        rb_tree_transplant(root, node, node->right);
    } else if (node->right == 0) {
        child = node->left;
        child_parent = rb_tree_parent(node);
        rb_tree_transplant(root, node, node->left);
    } else {
        moved = node->right;
        while (moved->left != 0) moved = moved->left;
        moved_color = rb_tree_color(moved);
        child = moved->right;
        if (rb_tree_parent(moved) == node) {
            child_parent = moved;
            rb_tree_set_parent(child, moved);
        } else {
            child_parent = rb_tree_parent(moved);
            rb_tree_transplant(root, moved, moved->right);
            moved->right = node->right;
            rb_tree_set_parent(moved->right, moved);
        }
        rb_tree_transplant(root, node, moved);
        moved->left = node->left;
        rb_tree_set_parent(moved->left, moved);
        rb_tree_set_color(moved, rb_tree_color(node));
    }
    /* 后继节点的原父节点现在可沿其新位置到达根节点。
     * 在平衡操作读取子树摘要前，先修复这条路径。 */
    if (propagate != 0 && child_parent != 0) propagate(child_parent);
    if (moved_color == RB_TREE_BLACK) {
        rb_tree_erase_rebalance_augmented(root, child, child_parent, rotate);
    }
    rb_tree_node_init(node);
}

static inline void rb_tree_erase(rb_root_t *root, rb_node_t *node) {
    rb_tree_erase_augmented(root, node, 0, 0);
}
