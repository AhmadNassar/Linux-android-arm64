// SPDX-License-Identifier: GPL-2.0+
/*
 * Read-only subset of Linux lib/xarray.c.
 * Copyright (c) 2017-2018 Microsoft Corporation
 * Copyright (c) 2018-2020 Oracle
 * Author: Matthew Wilcox <willy@infradead.org>
 * Source: Google common 098619d7de5518919c2cb3dec6cc832967029b04.
 * Names are prefixed; algorithms are unchanged. GKI does not export
 * xas_find_marked in its KMI lists. Only header-inline XArray APIs are used.
 * Callers hold RCU and retain enkernel's bounded retries and double sample.
 */
#ifndef SGAME_SIM_XARRAY_WALK_H
#define SGAME_SIM_XARRAY_WALK_H

static inline bool sgame_sim_xa_track_free(const struct xarray *xa)
{
	return xa->xa_flags & XA_FLAGS_TRACK_FREE;
}

static unsigned int sgame_sim_get_offset(unsigned long index, struct xa_node *node)
{
	return (index >> node->shift) & XA_CHUNK_MASK;
}

static void sgame_sim_xas_set_offset(struct xa_state *xas)
{
	xas->xa_offset = sgame_sim_get_offset(xas->xa_index, xas->xa_node);
}

static void sgame_sim_xas_move_index(struct xa_state *xas, unsigned long offset)
{
	unsigned int shift = xas->xa_node->shift;
	xas->xa_index &= ~XA_CHUNK_MASK << shift;
	xas->xa_index += offset << shift;
}

static void *sgame_sim_set_bounds(struct xa_state *xas)
{
	xas->xa_node = XAS_BOUNDS;
	return NULL;
}

static unsigned long sgame_sim_max_index(void *entry)
{
	if (!xa_is_node(entry))
		return 0;
	return (XA_CHUNK_SIZE << xa_to_node(entry)->shift) - 1;
}

static void *sgame_sim_xas_find_marked(struct xa_state *xas, unsigned long max, xa_mark_t mark)
{
	bool advance = true;
	unsigned int offset;
	void *entry;

	if (xas_error(xas))
		return NULL;
	if (xas->xa_index > max)
		goto max;

	if (!xas->xa_node) {
		xas->xa_index = 1;
		goto out;
	} else if (xas_top(xas->xa_node)) {
		advance = false;
		entry = xa_head(xas->xa);
		xas->xa_node = NULL;
		if (xas->xa_index > sgame_sim_max_index(entry))
			goto out;
		if (!xa_is_node(entry)) {
			if (xa_marked(xas->xa, mark))
				return entry;
			xas->xa_index = 1;
			goto out;
		}
		xas->xa_node = xa_to_node(entry);
		xas->xa_offset = xas->xa_index >> xas->xa_node->shift;
	}

	while (xas->xa_index <= max) {
		if (unlikely(xas->xa_offset == XA_CHUNK_SIZE)) {
			xas->xa_offset = xas->xa_node->offset + 1;
			xas->xa_node = xa_parent(xas->xa, xas->xa_node);
			if (!xas->xa_node)
				break;
			advance = false;
			continue;
		}

		if (!advance) {
			entry = xa_entry(xas->xa, xas->xa_node, xas->xa_offset);
			if (xa_is_sibling(entry)) {
				xas->xa_offset = xa_to_sibling(entry);
				sgame_sim_xas_move_index(xas, xas->xa_offset);
			}
		}

		offset = xas_find_chunk(xas, advance, mark);
		if (offset > xas->xa_offset) {
			advance = false;
			sgame_sim_xas_move_index(xas, offset);
			/* Mind the wrap */
			if ((xas->xa_index - 1) >= max)
				goto max;
			xas->xa_offset = offset;
			if (offset == XA_CHUNK_SIZE)
				continue;
		}

		entry = xa_entry(xas->xa, xas->xa_node, xas->xa_offset);
		if (!entry && !(sgame_sim_xa_track_free(xas->xa) && mark == XA_FREE_MARK))
			continue;
		if (!xa_is_node(entry))
			return entry;
		xas->xa_node = xa_to_node(entry);
		sgame_sim_xas_set_offset(xas);
	}

out:
	if (xas->xa_index > max)
		goto max;
	return sgame_sim_set_bounds(xas);
max:
	xas->xa_node = XAS_RESTART;
	return NULL;
}

#endif
