/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SVEC_H
#define _LINUX_SVEC_H

/*
 * svec — Swap Vector: unordered contiguous array with O(1) insert/delete.
 *
 * An intrusive data structure (like list_head) that maintains elements
 * in a contiguous pointer array.  Deletion uses swap-and-pop to preserve
 * contiguity without shifting.  Order is not preserved across deletions.
 *
 * Use when:
 *  - Contiguous storage enables prefetch or SIMD over elements
 *  - Element ordering is irrelevant (scoring, batch processing)
 *  - O(1) insert/delete is required
 *
 * Each element embeds a struct svec_node.  The svec_head holds an array
 * of svec_node pointers provided by the caller.
 *
 *   struct my_item {
 *       int data;
 *       struct svec_node sv_node;
 *   };
 *
 *   struct svec_node *buf[64];
 *   struct svec_head head;
 *   svec_init(&head, buf, 64);
 *
 *   svec_add(&head, &item->sv_node);        // O(1) append
 *   svec_del(&head, &item->sv_node);        // O(1) swap-and-pop
 *
 *   svec_for_each_entry(pos, &head, sv_node) {
 *       // iterate all elements contiguously
 *   }
 */

#include <linux/container_of.h>
#include <linux/types.h>
#include <linux/compiler.h>

struct svec_node {
	int svec_idx;		/* index in parent svec_head, -1 = detached */
};

struct svec_head {
	struct svec_node **svec_entries;	/* caller-provided pointer array */
	int svec_nr;			/* current element count */
	int svec_capacity;		/* maximum element count */
};

#define SVEC_NODE_INIT { .svec_idx = -1 }

#define SVEC_HEAD_INIT(name, storage, cap) {	\
	.svec_entries = (storage),		\
	.svec_nr = 0,				\
	.svec_capacity = (cap),			\
}

/**
 * DEFINE_SVEC - declare and initialize a svec_head with backing storage
 * @name: variable name for the svec_head
 * @cap:  fixed capacity (number of elements)
 */
#define DEFINE_SVEC(name, cap)						\
	struct svec_node *__##name##_buf[cap];				\
	struct svec_head name = SVEC_HEAD_INIT(name, __##name##_buf, cap)

static inline void svec_init(struct svec_head *head,
			     struct svec_node **storage, int capacity)
{
	head->svec_entries = storage;
	head->svec_nr = 0;
	head->svec_capacity = capacity;
}

static inline void svec_node_init(struct svec_node *node)
{
	node->svec_idx = -1;
}

static inline bool svec_node_attached(const struct svec_node *node)
{
	return node->svec_idx >= 0;
}

static inline int svec_count(const struct svec_head *head)
{
	return head->svec_nr;
}

static inline bool svec_empty(const struct svec_head *head)
{
	return head->svec_nr == 0;
}

static inline bool svec_full(const struct svec_head *head)
{
	return head->svec_nr >= head->svec_capacity;
}

/**
 * svec_add - append an element to the svec.  O(1).
 * @head: the svec_head
 * @node: the svec_node embedded in the element to add
 *
 * Returns 0 on success, -1 if the svec is full.
 * The node must not already be attached.
 */
static inline int svec_add(struct svec_head *head, struct svec_node *node)
{
	int idx;

	if (unlikely(head->svec_nr >= head->svec_capacity))
		return -1;

	idx = head->svec_nr++;
	head->svec_entries[idx] = node;
	node->svec_idx = idx;
	return 0;
}

/**
 * svec_del - remove an element via swap-and-pop.  O(1).
 * @head: the svec_head
 * @node: the svec_node to remove
 *
 * The last element is swapped into the deleted element's slot.
 * Order is not preserved.  The node is marked detached (idx = -1).
 */
static inline void svec_del(struct svec_head *head, struct svec_node *node)
{
	int idx = node->svec_idx;
	int last;

	if (unlikely(idx < 0))
		return;

	last = --head->svec_nr;
	if (idx != last) {
		struct svec_node *moved = head->svec_entries[last];

		head->svec_entries[idx] = moved;
		moved->svec_idx = idx;
	}
	node->svec_idx = -1;
}

/**
 * svec_entry - get the container struct from an svec_node pointer.
 * @ptr:    svec_node pointer
 * @type:   type of the container struct
 * @member: name of the svec_node member in the container
 */
#define svec_entry(ptr, type, member) \
	container_of(ptr, type, member)

/**
 * svec_entry_idx - get the container struct at a given index.
 * @head:   svec_head pointer
 * @idx:    index (0 .. svec_count-1)
 * @type:   type of the container struct
 * @member: name of the svec_node member
 */
#define svec_entry_idx(head, idx, type, member) \
	container_of((head)->svec_entries[idx], type, member)

/**
 * svec_for_each - iterate over svec_node pointers.
 * @pos:  svec_node pointer used as loop cursor
 * @head: svec_head pointer
 */
#define svec_for_each(pos, head)				\
	for (int __svec_i = 0;					\
	     __svec_i < (head)->svec_nr &&			\
		({ (pos) = (head)->svec_entries[__svec_i]; 1; });\
	     __svec_i++)

/**
 * svec_for_each_entry - iterate over container structs in an svec.
 * @pos:    container struct pointer used as loop cursor
 * @head:   svec_head pointer
 * @member: name of the svec_node member in the container
 */
#define svec_for_each_entry(pos, head, member)			\
	for (int __svec_i = 0;					\
	     __svec_i < (head)->svec_nr &&			\
		({ (pos) = svec_entry_idx(head, __svec_i,	\
			typeof(*(pos)), member); 1; });		\
	     __svec_i++)

/**
 * svec_node_at - raw access to the node pointer at index.
 * @head: svec_head pointer
 * @idx:  index
 *
 * No bounds check -- caller must ensure idx < svec_count(head).
 * Useful for manual loops with prefetch.
 */
static inline struct svec_node *svec_node_at(const struct svec_head *head,
					     int idx)
{
	return head->svec_entries[idx];
}

#endif /* _LINUX_SVEC_H */
