/* The DOM tree navigation interface */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "elinks.h"

#include "document/dom/node.h"
#include "document/dom/stack.h"
#include "util/memory.h"
#include "util/string.h"


/* Navigator states */

#define DOM_STACK_STATE_GRANULARITY	0x7
#define DOM_STACK_CALLBACKS_SIZE	(sizeof(dom_stack_callback_T) * DOM_NODES)

static inline struct dom_stack_state *
realloc_dom_stack_states(struct dom_stack_state **states, size_t size)
{
	return mem_align_alloc(states, size, size + 1,
			       struct dom_stack_state,
			       DOM_STACK_STATE_GRANULARITY);
}

static inline unsigned char *
realloc_dom_stack_state_objects(struct dom_stack *stack)
{
#ifdef DEBUG_MEMLEAK
	return mem_align_alloc__(__FILE__, __LINE__, (void **) &stack->state_objects,
			       stack->depth, stack->depth + 1,
			       stack->object_size,
			       DOM_STACK_STATE_GRANULARITY);
#else
	return mem_align_alloc__((void **) &stack->state_objects,
			       stack->depth, stack->depth + 1,
			       stack->object_size,
			       DOM_STACK_STATE_GRANULARITY);
#endif
}

void
init_dom_stack(struct dom_stack *stack, void *data,
	       dom_stack_callback_T callbacks[DOM_NODES],
	       size_t object_size)
{
	assert(stack);

	memset(stack, 0, sizeof(*stack));

	stack->data        = data;
	stack->object_size = object_size;

	if (callbacks)
		memcpy(stack->callbacks, callbacks, DOM_STACK_CALLBACKS_SIZE);
}

void
done_dom_stack(struct dom_stack *stack)
{
	assert(stack);

	mem_free_if(stack->states);
	mem_free_if(stack->state_objects);

	memset(stack, 0, sizeof(*stack));
}

struct dom_node *
push_dom_node(struct dom_stack *stack, struct dom_node *node)
{
	dom_stack_callback_T callback;
	struct dom_stack_state *state;

	assert(stack && node);
	assert(0 < node->type && node->type < DOM_NODES);

	if (stack->depth > DOM_STACK_MAX_DEPTH) {
		return NULL;
	}

	state = realloc_dom_stack_states(&stack->states, stack->depth);
	if (!state) {
		done_dom_node(node);
		return NULL;
	}

	state += stack->depth;

	if (stack->object_size) {
		unsigned char *state_objects;
		size_t offset = stack->depth * stack->object_size;

		state_objects = realloc_dom_stack_state_objects(stack);
		if (!state_objects) {
			done_dom_node(node);
			return NULL;
		}

		state->data = (void *) &state_objects[offset];
	}

	state->node = node;

	/* Grow the state array to the new depth so the state accessors work
	 * in the callbacks */
	stack->depth++;

	callback = stack->callbacks[node->type];
	if (callback) {
		node = callback(stack, node, state->data);

		/* If the callback returned NULL pop the state immediately */
		if (!node) {
			memset(state, 0, sizeof(*state));
			stack->depth--;
			assert(stack->depth >= 0);
		}
	}

	return node;
}

static int
do_pop_dom_node(struct dom_stack *stack, struct dom_stack_state *parent)
{
	struct dom_stack_state *state;

	assert(stack);
	if (!dom_stack_has_parents(stack)) return 0;

	state = get_dom_stack_top(stack);
	if (state->callback) {
		/* Pass the node we are popping to and _not_ the state->node */
		state->callback(stack, parent->node, state->data);
	}

	stack->depth--;
	assert(stack->depth >= 0);

	if (stack->object_size && state->data) {
		size_t offset = stack->depth * stack->object_size;

		/* I tried to use item->data here but it caused a memory
		 * corruption bug on fm. This is also less trustworthy in that
		 * the state->data pointer could have been mangled. --jonas */
		memset(&stack->state_objects[offset], 0, stack->object_size);
	}

	memset(state, 0, sizeof(*state));

	return state == parent;
}

void
pop_dom_node(struct dom_stack *stack)
{
	assert(stack);
	if (!dom_stack_has_parents(stack)) return;

	do_pop_dom_node(stack, get_dom_stack_parent(stack));
}

void
pop_dom_nodes(struct dom_stack *stack, enum dom_node_type type,
	      unsigned char *string, uint16_t length)
{
	struct dom_stack_state *state, *parent;
	unsigned int pos;

	if (!dom_stack_has_parents(stack)) return;

	parent = search_dom_stack(stack, type, string, length);
	if (!parent) return;

	foreachback_dom_state (stack, state, pos) {
		if (do_pop_dom_node(stack, parent))
			break;;
	}
}

void
walk_dom_nodes(struct dom_stack *stack, struct dom_node *root)
{
	assert(root && stack);

	push_dom_node(stack, root);

	while (dom_stack_has_parents(stack)) {
		struct dom_stack_state *state = get_dom_stack_top(stack);
		struct dom_node_list *list = state->list;
		struct dom_node *node = state->node;

		switch (node->type) {
		case DOM_NODE_DOCUMENT:
			if (!list) list = node->data.document.children;
			break;

		case DOM_NODE_ELEMENT:
			if (!list) list = node->data.element.map;

			if (list == node->data.element.children) break;
			if (is_dom_node_list_member(list, state->index)
			    && list == node->data.element.map)
				break;

			list = node->data.element.children;
			break;

		case DOM_NODE_PROCESSING_INSTRUCTION:
			if (!list) list = node->data.proc_instruction.map;
			break;

		case DOM_NODE_DOCUMENT_TYPE:
			if (!list) list = node->data.document_type.entities;

			if (list == node->data.document_type.notations) break;
			if (is_dom_node_list_member(list, state->index)
			    && list == node->data.document_type.entities)
				break;

			list = node->data.document_type.notations;
			break;

		case DOM_NODE_ATTRIBUTE:
		case DOM_NODE_TEXT:
		case DOM_NODE_CDATA_SECTION:
		case DOM_NODE_COMMENT:
		case DOM_NODE_NOTATION:
		case DOM_NODE_DOCUMENT_FRAGMENT:
		case DOM_NODE_ENTITY_REFERENCE:
		case DOM_NODE_ENTITY:
		default:
			break;
		}

		/* Reset list state if it is a new list */
		if (list != state->list) {
			state->list  = list;
			state->index = 0;
		}

		/* If we have next child node */
		if (is_dom_node_list_member(list, state->index)) {
			struct dom_node *child = list->entries[state->index++];

			if (push_dom_node(stack, child))
				continue;
		}

		pop_dom_node(stack);
	}
}