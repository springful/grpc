/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/channel/channel_stack.h"
#include <grpc/support/log.h>

#include <stdlib.h>
#include <string.h>

int grpc_trace_channel = 0;

/* Memory layouts.

   Channel stack is laid out as: {
     grpc_channel_stack stk;
     padding to GPR_MAX_ALIGNMENT
     grpc_channel_element[stk.count];
     per-filter memory, aligned to GPR_MAX_ALIGNMENT
   }

   Call stack is laid out as: {
     grpc_call_stack stk;
     padding to GPR_MAX_ALIGNMENT
     grpc_call_element[stk.count];
     per-filter memory, aligned to GPR_MAX_ALIGNMENT
   } */

/* Given a size, round up to the next multiple of sizeof(void*) */
#define ROUND_UP_TO_ALIGNMENT_SIZE(x) \
  (((x) + GPR_MAX_ALIGNMENT - 1) & ~(GPR_MAX_ALIGNMENT - 1))

size_t grpc_channel_stack_size(const grpc_channel_filter **filters,
                               size_t filter_count) {
  /* always need the header, and size for the channel elements */
  size_t size =
      ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(grpc_channel_stack)) +
      ROUND_UP_TO_ALIGNMENT_SIZE(filter_count * sizeof(grpc_channel_element));
  size_t i;

  GPR_ASSERT((GPR_MAX_ALIGNMENT & (GPR_MAX_ALIGNMENT - 1)) == 0 &&
             "GPR_MAX_ALIGNMENT must be a power of two");

  /* add the size for each filter */
  for (i = 0; i < filter_count; i++) {
    size += ROUND_UP_TO_ALIGNMENT_SIZE(filters[i]->sizeof_channel_data);
  }

  return size;
}

#define CHANNEL_ELEMS_FROM_STACK(stk)                                   \
  ((grpc_channel_element *)((char *)(stk) + ROUND_UP_TO_ALIGNMENT_SIZE( \
                                                sizeof(grpc_channel_stack))))

#define CALL_ELEMS_FROM_STACK(stk)       \
  ((grpc_call_element *)((char *)(stk) + \
                         ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(grpc_call_stack))))

grpc_channel_element *grpc_channel_stack_element(
    grpc_channel_stack *channel_stack, size_t index) {
  return CHANNEL_ELEMS_FROM_STACK(channel_stack) + index;
}

grpc_channel_element *grpc_channel_stack_last_element(
    grpc_channel_stack *channel_stack) {
  return grpc_channel_stack_element(channel_stack, channel_stack->count - 1);
}

grpc_call_element *grpc_call_stack_element(grpc_call_stack *call_stack,
                                           size_t index) {
  return CALL_ELEMS_FROM_STACK(call_stack) + index;
}

void grpc_channel_stack_init(const grpc_channel_filter **filters,
                             size_t filter_count, const grpc_channel_args *args,
                             grpc_mdctx *metadata_context,
                             grpc_channel_stack *stack) {
  size_t call_size =
      ROUND_UP_TO_ALIGNMENT_SIZE(sizeof(grpc_call_stack)) +
      ROUND_UP_TO_ALIGNMENT_SIZE(filter_count * sizeof(grpc_call_element));
  grpc_channel_element *elems;
  char *user_data;
  size_t i;

  stack->count = filter_count;
  elems = CHANNEL_ELEMS_FROM_STACK(stack);
  user_data =
      ((char *)elems) +
      ROUND_UP_TO_ALIGNMENT_SIZE(filter_count * sizeof(grpc_channel_element));

  /* init per-filter data */
  for (i = 0; i < filter_count; i++) {
    elems[i].filter = filters[i];
    elems[i].channel_data = user_data;
    elems[i].filter->init_channel_elem(&elems[i], args, metadata_context,
                                       i == 0, i == (filter_count - 1));
    user_data += ROUND_UP_TO_ALIGNMENT_SIZE(filters[i]->sizeof_channel_data);
    call_size += ROUND_UP_TO_ALIGNMENT_SIZE(filters[i]->sizeof_call_data);
  }

  GPR_ASSERT(user_data > (char *)stack);
  GPR_ASSERT((gpr_uintptr)(user_data - (char *)stack) ==
             grpc_channel_stack_size(filters, filter_count));

  stack->call_stack_size = call_size;
}

void grpc_channel_stack_destroy(grpc_channel_stack *stack) {
  grpc_channel_element *channel_elems = CHANNEL_ELEMS_FROM_STACK(stack);
  size_t count = stack->count;
  size_t i;

  /* destroy per-filter data */
  for (i = 0; i < count; i++) {
    channel_elems[i].filter->destroy_channel_elem(&channel_elems[i]);
  }
}

void grpc_call_stack_init(grpc_channel_stack *channel_stack,
                          const void *transport_server_data,
                          grpc_transport_op *initial_op,
                          grpc_call_stack *call_stack) {
  grpc_channel_element *channel_elems = CHANNEL_ELEMS_FROM_STACK(channel_stack);
  size_t count = channel_stack->count;
  grpc_call_element *call_elems;
  char *user_data;
  size_t i;

  call_stack->count = count;
  call_elems = CALL_ELEMS_FROM_STACK(call_stack);
  user_data = ((char *)call_elems) +
              ROUND_UP_TO_ALIGNMENT_SIZE(count * sizeof(grpc_call_element));

  /* init per-filter data */
  for (i = 0; i < count; i++) {
    call_elems[i].filter = channel_elems[i].filter;
    call_elems[i].channel_data = channel_elems[i].channel_data;
    call_elems[i].call_data = user_data;
    call_elems[i].filter->init_call_elem(&call_elems[i], transport_server_data,
                                         initial_op);
    user_data +=
        ROUND_UP_TO_ALIGNMENT_SIZE(call_elems[i].filter->sizeof_call_data);
  }
}

void grpc_call_stack_destroy(grpc_call_stack *stack) {
  grpc_call_element *elems = CALL_ELEMS_FROM_STACK(stack);
  size_t count = stack->count;
  size_t i;

  /* destroy per-filter data */
  for (i = 0; i < count; i++) {
    elems[i].filter->destroy_call_elem(&elems[i]);
  }
}

void grpc_call_next_op(grpc_call_element *elem, grpc_transport_op *op) {
  grpc_call_element *next_elem = elem + 1;
  next_elem->filter->start_transport_op(next_elem, op);
}

void grpc_channel_next_op(grpc_channel_element *elem, grpc_channel_op *op) {
  grpc_channel_element *next_elem = elem + op->dir;
  next_elem->filter->channel_op(next_elem, elem, op);
}

grpc_channel_stack *grpc_channel_stack_from_top_element(
    grpc_channel_element *elem) {
  return (grpc_channel_stack *)((char *)(elem)-ROUND_UP_TO_ALIGNMENT_SIZE(
      sizeof(grpc_channel_stack)));
}

grpc_call_stack *grpc_call_stack_from_top_element(grpc_call_element *elem) {
  return (grpc_call_stack *)((char *)(elem)-ROUND_UP_TO_ALIGNMENT_SIZE(
      sizeof(grpc_call_stack)));
}

void grpc_call_element_send_cancel(grpc_call_element *cur_elem) {
  grpc_transport_op op;
  memset(&op, 0, sizeof(op));
  op.cancel_with_status = GRPC_STATUS_CANCELLED;
  grpc_call_next_op(cur_elem, &op);
}

void grpc_call_element_recv_status(grpc_call_element *cur_elem,
                                   grpc_status_code status,
                                   const char *message) {
  abort();
}
