/*
 * QuickJS Javascript Engine
 *
 * Copyright (c) 2017-2021 Fabrice Bellard
 * Copyright (c) 2017-2021 Charlie Gordon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "ic.h"

static force_inline uint32_t get_index_hash(JSAtom atom, int hash_bits) {
  return (atom * 0x9e370001) >> (32 - hash_bits);
}

InlineCache *init_ic(JSContext *ctx) {
  InlineCache *ic;
  ic = js_malloc(ctx, sizeof(InlineCache));
  if (unlikely(!ic))
    goto fail;
  ic->count = 0;
  ic->hash_bits = 2;
  ic->capacity = 1 << ic->hash_bits;
  ic->ctx = ctx;
  ic->hash = js_malloc(ctx, sizeof(ic->hash[0]) * ic->capacity);
  if (unlikely(!ic->hash))
    goto fail;
  memset(ic->hash, 0, sizeof(ic->hash[0]) * ic->capacity);
  ic->cache = NULL;
  ic->updated = FALSE;
  ic->updated_offset = 0;
  return ic;
fail:
  return NULL;
}

int rebuild_ic(InlineCache *ic) {
  uint32_t i, count;
  InlineCacheHashSlot *ch;
  if (ic->count == 0)
    goto end;
  count = 0;
  ic->cache = js_malloc(ic->ctx, sizeof(InlineCacheRingSlot) * ic->count);
  if (unlikely(!ic->cache))
    goto fail;
  memset(ic->cache, 0, sizeof(InlineCacheRingSlot) * ic->count);
  for (i = 0; i < ic->capacity; i++) {
    for (ch = ic->hash[i]; ch != NULL; ch = ch->next) {
      ch->index = count++;
      ic->cache[ch->index].atom = JS_DupAtom(ic->ctx, ch->atom);
      ic->cache[ch->index].index = 0;
    }
  }
end:
  return 0;
fail:
  return -1;
}

int resize_ic_hash(InlineCache *ic) {
  uint32_t new_capacity, i, h;
  InlineCacheHashSlot *ch, *ch_next;
  InlineCacheHashSlot **new_hash;
  ic->hash_bits += 1;
  new_capacity = 1 << ic->hash_bits;
  new_hash = js_malloc(ic->ctx, sizeof(ic->hash[0]) * new_capacity);
  if (unlikely(!new_hash))
    goto fail;
  memset(new_hash, 0, sizeof(ic->hash[0]) * new_capacity);
  for (i = 0; i < ic->capacity; i++) {
    for (ch = ic->hash[i]; ch != NULL; ch = ch_next) {
      h = get_index_hash(ch->atom, ic->hash_bits);
      ch_next = ch->next;
      ch->next = new_hash[h];
      new_hash[h] = ch;
    }
  }
  js_free(ic->ctx, ic->hash);
  ic->hash = new_hash;
  ic->capacity = new_capacity;
  return 0;
fail:
  return -1;
}

int free_ic(InlineCache *ic) {
  uint32_t i, j;
  InlineCacheHashSlot *ch, *ch_next;
  InlineCacheRingItem *buffer;
  for (i = 0; i < ic->count; i++) {
    buffer = ic->cache[i].buffer;
    JS_FreeAtom(ic->ctx, ic->cache[i].atom);
    for (j = 0; j < IC_CACHE_ITEM_CAPACITY; j++) {
      js_shape_clear_watchpoints(ic->ctx->rt, buffer[j].shape);
      js_free_shape_null(ic->ctx->rt, buffer[j].shape);
    }
  }
  for (i = 0; i < ic->capacity; i++) {
    for (ch = ic->hash[i]; ch != NULL; ch = ch_next) {
      ch_next = ch->next;
      JS_FreeAtom(ic->ctx, ch->atom);
      js_free(ic->ctx, ch);
    }
  }
  if (ic->count > 0)
    js_free(ic->ctx, ic->cache);
  js_free(ic->ctx, ic->hash);
  js_free(ic->ctx, ic);
  return 0;
}

uint32_t add_ic_slot(InlineCache *ic, JSAtom atom, JSObject *object,
                     uint32_t prop_offset, JSObject* prototype) {
  int32_t i;
  uint32_t h;
  InlineCacheHashSlot *ch;
  InlineCacheRingSlot *cr;
  InlineCacheRingItem *ci;
  JSRuntime* rt;
  JSShape *sh, *sh1;
  JSObject *proto;
  cr = NULL;
  rt = ic->ctx->rt;
  sh = NULL;
  proto = NULL;
  h = get_index_hash(atom, ic->hash_bits);
  for (ch = ic->hash[h]; ch != NULL; ch = ch->next)
    if (ch->atom == atom) {
      cr = ic->cache + ch->index;
      break;
    }

  assert(cr != NULL);
  i = cr->index;
  for (;;) {
    if (object->shape == cr->buffer[i].shape)
      break;

    i = (i + 1) % IC_CACHE_ITEM_CAPACITY;
    if (unlikely(i == cr->index))
      break;
  }

  ci = cr->buffer + i;
  sh = ci->shape;
  sh1 = js_dup_shape(object->shape);
  js_free_shape_null(rt, sh);
  if (ci->watchpoint_ref)
    js_shape_remove_watchpoints(rt, sh, ci);
  ci->prop_offset = prop_offset;
  ci->shape = sh1;
  if (prototype) {
    // the atom and prototype SHOULE BE freed by watchpoint_remove/clear_callback
    JS_DupAtom(ic->ctx, atom);
    JS_DupValue(ic->ctx, JS_MKPTR(JS_TAG_OBJECT, prototype));
    ci->proto = prototype;
    ci->watchpoint_ref = js_shape_create_watchpoint(rt, ci->shape, (intptr_t)ci, &atom,
                          ic_watchpoint_remove_callback,
                          ic_watchpoint_clear_callback);
  }
  return ch->index;
}

uint32_t add_ic_slot1(InlineCache *ic, JSAtom atom) {
  uint32_t h;
  InlineCacheHashSlot *ch;
  if (ic->count + 1 >= ic->capacity && resize_ic_hash(ic))
    goto end;
  h = get_index_hash(atom, ic->hash_bits);
  for (ch = ic->hash[h]; ch != NULL; ch = ch->next)
    if (ch->atom == atom)
      goto end;
  ch = js_malloc(ic->ctx, sizeof(InlineCacheHashSlot));
  if (unlikely(!ch))
    goto end;
  ch->atom = JS_DupAtom(ic->ctx, atom);
  ch->index = 0;
  ch->next = ic->hash[h];
  ic->hash[h] = ch;
  ic->count += 1;
end:
  return 0;
}

int ic_watchpoint_remove_callback(JSRuntime* rt, intptr_t ref, void* extra_data, void* target) {
  InlineCacheRingItem *ci;
  JSAtom *atom;
  ci = (InlineCacheRingItem *)ref;
  atom = (JSAtom *)extra_data;
  if(ref != (intptr_t)target)
    return 1;
  assert(ci->proto != NULL);
  // the shape and prop_offset WILL BE handled by add_ic_slot
  // !!! MUST NOT CALL js_free_shape0 TO DOUBLE FREE HERE !!!
  JS_FreeAtomRT(rt, *atom);
  JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, ci->proto));
  ci->watchpoint_ref = NULL;
  ci->proto = NULL;
  ci->prop_offset = 0;
  ci->shape = NULL;
  return 0;
}

int ic_watchpoint_clear_callback(JSRuntime* rt, intptr_t ref, void* extra_data) {
  InlineCacheRingItem *ci;
  JSAtom *atom;
  ci = (InlineCacheRingItem *)ref;
  assert(ci->watchpoint_ref != NULL);
  assert(ci->proto != NULL);
  // the watchpoint_clear_callback ONLY CAN BE called by js_free_shape0
  // !!! MUST NOT CALL js_free_shape0 TO DOUBLE FREE HERE !!!
  JS_FreeAtomRT(rt, *atom);
  JS_FreeValueRT(rt, JS_MKPTR(JS_TAG_OBJECT, ci->proto));
  ci->watchpoint_ref = NULL;
  ci->proto = NULL;
  ci->prop_offset = 0;
  ci->shape = NULL;
  return 0;
}

int ic_remove_shape_proto_watchpoints(JSRuntime *rt, JSShape *shape, JSAtom atom) {
  ObjectWatchpoint *o, *watchpoint;
  JSObject *p;
  JSAtom *prop;
  InlineCacheRingItem *ci;
  p = shape->proto;
  while(p) {
    watchpoint = p->shape->watchpoint;
    while(watchpoint) {
      o = watchpoint->next;
      prop = (JSAtom *)watchpoint->extra_data;
      if(atom == *prop) {
        ci = (InlineCacheRingItem *)watchpoint->ref;
        watchpoint->remove_callback = NULL;
        watchpoint->clear_callback = NULL;
        ic_watchpoint_clear_callback(rt, watchpoint->ref, watchpoint->extra_data);
        js_free_shape_null(rt, ci->shape);
        if (o)
          o->prev = watchpoint->prev;
        if (watchpoint->prev)
          watchpoint->prev->next = o;
        else
          p->shape->watchpoint = NULL;
        js_free_rt(rt, watchpoint);
      }
      watchpoint = o;
    }
    p = p->shape->proto;
  }
  return 0;
}

int ic_clear_shape_proto_watchpoints(JSRuntime *rt, JSShape *shape) {
  ObjectWatchpoint *o, *watchpoint;
  JSObject *p;
  JSAtom *prop;
  InlineCacheRingItem *ci;
  p = shape->proto;
  while(p) {
    watchpoint = p->shape->watchpoint;
    p->shape->watchpoint = NULL;
    while(watchpoint) {
      o = watchpoint;
      watchpoint = o->next;
      prop = (JSAtom *)o->extra_data;
      ci = (InlineCacheRingItem *)o->ref;
      o->remove_callback = NULL;
      o->clear_callback = NULL;
      ic_watchpoint_clear_callback(rt, o->ref, o->extra_data);
      js_free_shape_null(rt, ci->shape);
      js_free_rt(rt, o);
    }
    p = p->shape->proto;
  }
  return 0;
}