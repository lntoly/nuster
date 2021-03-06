/*
 * nuster dict functions.
 *
 * Copyright (C) Jiang Wenyuan, < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <types/global.h>

#include <nuster/nuster.h>

int
nst_dict_init(nst_dict_t *dict, nst_memory_t *memory, uint64_t dict_size) {

    int  block_size = memory->block_size;
    int  entry_size = sizeof(nst_dict_entry_t *);
    int  size       = (block_size + dict_size - 1) / block_size * block_size;
    int  i;

    dict->memory = memory;
    dict->size   = size / entry_size;
    dict->used   = 0;
    dict->entry  = nst_memory_alloc(memory, block_size);

    if(!dict->entry) {
        return NST_ERR;
    }

    for(i = 1; i < size / block_size; i++) {

        if(!nst_memory_alloc(memory, block_size)) {
            return NST_ERR;
        }
    }

    for(i = 0; i < dict->size; i++) {
        dict->entry[i] = NULL;
    }

    return nst_shctx_init(dict);
}

/*
 * Check entry validity, free the entry if its invalid,
 */
void
nst_dict_cleanup(nst_dict_t *dict) {
    nst_dict_entry_t  *entry;
    nst_dict_entry_t  *prev;

    if(!dict->used) {
        return;
    }

    nst_shctx_lock(dict);

    entry = dict->entry[dict->cleanup_idx];
    prev  = entry;

    while(entry) {

        if(nst_dict_entry_invalid(entry)) {
            nst_dict_entry_t  *tmp = entry;

            if(entry->store.ring.data) {
                entry->store.ring.data->invalid = 1;
                entry->store.ring.data = NULL;
            }

            if(entry->store.disk.file) {
                nst_memory_free(dict->memory, entry->store.disk.file);
                entry->store.disk.file = NULL;
            }

            if(prev == entry) {
                dict->entry[dict->cleanup_idx] = entry->next;
                prev = entry->next;
            } else {
                prev->next = entry->next;
            }

            entry = entry->next;

            nst_memory_free(dict->memory, tmp->buf.area);
            nst_memory_free(dict->memory, tmp->key.data);
            nst_memory_free(dict->memory, tmp);

            dict->used--;
        } else {
            prev  = entry;
            entry = entry->next;
        }
    }

    dict->cleanup_idx++;

    /* if we have checked the whole dict */
    if(dict->cleanup_idx == dict->size) {
        dict->cleanup_idx = 0;
    }

    nst_shctx_unlock(dict);
}

nst_dict_entry_t *
nst_dict_set(nst_dict_t *dict, nst_key_t *key, nst_http_txn_t *txn, nst_rule_t *rule, int pid) {

    nst_dict_entry_t  *entry = NULL;
    int                idx;

    entry = nst_memory_alloc(dict->memory, sizeof(*entry));

    if(!entry) {
        goto err;
    }

    memset(entry, 0, sizeof(*entry));

    idx = key->hash % dict->size;

    /* prepend entry to dict->entry[idx] */
    entry->next      = dict->entry[idx];
    dict->entry[idx] = entry;
    dict->used++;

    /* init entry */
    entry->state = NST_DICT_ENTRY_STATE_INIT;

    /* set key */
    entry->key.size = key->size;
    entry->key.hash = key->hash;
    entry->key.data = nst_memory_alloc(dict->memory, key->size);

    if(!entry->key.data) {
        goto err;
    }

    memcpy(entry->key.data, key->data, key->size);
    memcpy(entry->key.uuid, key->uuid, 20);

    /* set buf */
    entry->buf.size = txn->buf->data;
    entry->buf.data = txn->buf->data;
    entry->buf.area = nst_memory_alloc(dict->memory, entry->buf.size);

    if(!entry->buf.area) {
        goto err;
    }

    memcpy(entry->buf.area, txn->buf->area, txn->buf->data);

    entry->host.ptr          = entry->buf.area + (txn->req.host.ptr - txn->buf->area);
    entry->host.len          = txn->req.host.len;
    entry->path.ptr          = entry->buf.area + (txn->req.path.ptr - txn->buf->area);
    entry->path.len          = txn->req.path.len;
    entry->etag.ptr          = entry->buf.area + (txn->res.etag.ptr - txn->buf->area);
    entry->etag.len          = txn->res.etag.len;
    entry->last_modified.ptr = entry->buf.area + (txn->res.last_modified.ptr - txn->buf->area);
    entry->last_modified.len = txn->res.last_modified.len;
    entry->rule              = rule;
    entry->expire            = 0;
    entry->pid               = pid;
    entry->ttl               = rule->ttl;
    entry->extend[0]         = rule->extend[0];
    entry->extend[1]         = rule->extend[1];
    entry->extend[2]         = rule->extend[2];
    entry->extend[3]         = rule->extend[3];
    entry->header_len        = txn->res.header_len;

    return entry;

err:

    if(entry) {
        entry->state = NST_DICT_ENTRY_STATE_INVALID;
    }

    return NULL;
}

/*
 * return NULL if invalid;
 * return entry if init and valid
 */
nst_dict_entry_t *
nst_dict_get(nst_dict_t *dict, nst_key_t *key) {
    nst_dict_entry_t  *entry = NULL;
    uint64_t           max;
    int                idx, expired;

    if(dict->used == 0) {
        return NULL;
    }

    idx   = key->hash % dict->size;
    entry = dict->entry[idx];

    while(entry) {

        if(entry->key.hash == key->hash && entry->key.size == key->size
                && !memcmp(entry->key.uuid, key->uuid, NST_KEY_UUID_LEN)
                && !memcmp(entry->key.data, key->data, key->size)) {

            if(entry->state == NST_DICT_ENTRY_STATE_INVALID) {
                return NULL;
            }

            if(entry->state == NST_DICT_ENTRY_STATE_INIT
                    || entry->state == NST_DICT_ENTRY_STATE_UPDATE) {

                return entry;
            }

            expired  = nst_dict_entry_expired(entry);

            max = 1000 * entry->expire + 1000 * entry->ttl * entry->extend[3] / 100;

            entry->atime = get_current_timestamp();

            if(expired && entry->extend[0] != 0xFF && entry->atime <= max
                    && entry->access[3] > entry->access[2]
                    && entry->access[2] > entry->access[1]) {

                entry->expire    += entry->ttl;

                entry->access[0] += entry->access[1];
                entry->access[0] += entry->access[2];
                entry->access[0] += entry->access[3];
                entry->access[1]  = 0;
                entry->access[2]  = 0;
                entry->access[3]  = 0;
                entry->extended  += 1;

                if(entry->store.disk.file) {
                    nst_disk_update_expire(entry->store.disk.file, entry->expire);
                }

                expired = 0;
            }

            /* check expire
             * change state only, leave the free stuff to cleanup
             * */
            if(entry->state == NST_DICT_ENTRY_STATE_VALID && expired) {
                entry->state     = NST_DICT_ENTRY_STATE_INVALID;
                entry->expire    = 0;
                entry->access[0] = 0;
                entry->access[1] = 0;
                entry->access[2] = 0;
                entry->access[3] = 0;
                entry->extended  = 0;

                if(entry->store.ring.data) {
                    entry->store.ring.data->invalid = 1;
                    entry->store.ring.data          = NULL;
                }

                return NULL;
            }

            return entry;
        }

        entry = entry->next;
    }

    return NULL;
}

int
nst_dict_set_from_disk(nst_dict_t *dict, hpx_buffer_t *buf, hpx_ist_t host, hpx_ist_t path,
        nst_key_t *key, char *file, char *meta) {

    nst_dict_entry_t  *entry = NULL;
    uint64_t           ttl_extend;
    int                idx;

    key->hash = nst_disk_meta_get_hash(meta);

    ttl_extend = nst_disk_meta_get_ttl_extend(meta);

    entry = nst_memory_alloc(dict->memory, sizeof(*entry));

    if(!entry) {
        return NST_ERR;
    }

    memset(entry, 0, sizeof(*entry));

    idx = key->hash % dict->size;

    /* prepend entry to dict->entry[idx] */
    entry->next      = dict->entry[idx];
    dict->entry[idx] = entry;
    dict->used++;

    /* init entry */
    entry->state  = NST_DICT_ENTRY_STATE_VALID;
    entry->key    = *key;
    entry->expire = nst_disk_meta_get_expire(meta);
    entry->store.disk.file   = nst_memory_alloc(dict->memory, strlen(file));

    if(!entry->store.disk.file) {
        nst_memory_free(dict->memory, entry);

        return NST_ERR;
    }

    memcpy(entry->store.disk.file, file, strlen(file));

    entry->header_len = nst_disk_meta_get_header_len(meta);

    entry->buf = *buf;

    entry->host = host;
    entry->path = path;

    entry->extend[0] = *( uint8_t *)(&ttl_extend);
    entry->extend[1] = *((uint8_t *)(&ttl_extend) + 1);
    entry->extend[2] = *((uint8_t *)(&ttl_extend) + 2);
    entry->extend[3] = *((uint8_t *)(&ttl_extend) + 3);

    entry->ttl = ttl_extend >> 32;

    return NST_OK;
}
