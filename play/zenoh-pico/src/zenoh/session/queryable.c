/*
 * Copyright (c) 2017, 2021 ADLINK Technology Inc.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Apache License, Version 2.0
 * which is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
 *
 * Contributors:
 *   ADLINK zenoh team, <zenoh@adlink-labs.tech>
 */

#include "zenoh-pico/protocol/private/msg.h"
#include "zenoh-pico/protocol/private/msgcodec.h"
#include "zenoh-pico/protocol/private/utils.h"
#include "zenoh-pico/protocol/utils.h"
#include "zenoh-pico/session/private/queryable.h"
#include "zenoh-pico/session/private/resource.h"
#include "zenoh-pico/session/private/types.h"
#include "zenoh-pico/system/common.h"
#include "zenoh-pico/transport/private/utils.h"
#include "zenoh-pico/utils/private/logging.h"

/*------------------ Queryable ------------------*/
/**
 * This function is unsafe because it operates in potentially concurrent data.
 * Make sure that the following mutexes are locked before calling this function:
 *  - zn->mutex_inner
 */
_zn_queryable_t *__unsafe_zn_get_queryable_by_id(zn_session_t *zn, z_zint_t id)
{
    z_list_t *queryables = zn->local_queryables;
    while (queryables)
    {
        _zn_queryable_t *queryable = (_zn_queryable_t *)z_list_head(queryables);

        if (queryable->id == id)
            return queryable;

        queryables = z_list_tail(queryables);
    }

    return NULL;
}

/**
 * This function is unsafe because it operates in potentially concurrent data.
 * Make sure that the following mutexes are locked before calling this function:
 *  - zn->mutex_inner
 */
z_list_t *__unsafe_zn_get_queryables_from_remote_key(zn_session_t *zn, const zn_reskey_t *reskey)
{
    z_list_t *xs = z_list_empty;
    // Case 1) -> numerical only reskey
    if (reskey->rname == NULL)
    {
        z_list_t *qles = (z_list_t *)z_i_map_get(zn->rem_res_loc_qle_map, reskey->rid);
        while (qles)
        {
            _zn_queryable_t *qle = (_zn_queryable_t *)z_list_head(qles);
            xs = z_list_cons(xs, qle);
            qles = z_list_tail(qles);
        }
    }
    // Case 2) -> string only reskey
    else if (reskey->rid == ZN_RESOURCE_ID_NONE)
    {
        // The complete resource name of the remote key
        z_str_t rname = reskey->rname;

        z_list_t *qles = zn->local_queryables;
        while (qles)
        {
            _zn_queryable_t *qle = (_zn_queryable_t *)z_list_head(qles);

            // The complete resource name of the subscribed key
            z_str_t lname;
            if (qle->key.rid == ZN_RESOURCE_ID_NONE)
            {
                // Do not allocate
                lname = qle->key.rname;
            }
            else
            {
                // Allocate a computed string
                lname = __unsafe_zn_get_resource_name_from_key(zn, _ZN_IS_LOCAL, &qle->key);
                if (lname == NULL)
                {
                    z_list_free(xs);
                    xs = NULL;
                    return xs;
                }
            }

            if (zn_rname_intersect(lname, rname))
                xs = z_list_cons(xs, qle);

            if (qle->key.rid != ZN_RESOURCE_ID_NONE)
                free(lname);

            qles = z_list_tail(qles);
        }
    }
    // Case 3) -> numerical reskey with suffix
    else
    {
        _zn_resource_t *remote = __unsafe_zn_get_resource_by_id(zn, _ZN_IS_REMOTE, reskey->rid);
        if (remote == NULL)
            return xs;

        // Compute the complete remote resource name starting from the key
        z_str_t rname = __unsafe_zn_get_resource_name_from_key(zn, _ZN_IS_REMOTE, reskey);

        z_list_t *qles = zn->local_queryables;
        while (qles)
        {
            _zn_queryable_t *qle = (_zn_queryable_t *)z_list_head(qles);

            // Get the complete resource name to be passed to the subscription callback
            z_str_t lname;
            if (qle->key.rid == ZN_RESOURCE_ID_NONE)
            {
                // Do not allocate
                lname = qle->key.rname;
            }
            else
            {
                // Allocate a computed string
                lname = __unsafe_zn_get_resource_name_from_key(zn, _ZN_IS_LOCAL, &qle->key);
                if (lname == NULL)
                    continue;
            }

            if (zn_rname_intersect(lname, rname))
                xs = z_list_cons(xs, qle);

            if (qle->key.rid != ZN_RESOURCE_ID_NONE)
                free(lname);

            qles = z_list_tail(qles);
        }

        free(rname);
    }

    return xs;
}

/**
 * This function is unsafe because it operates in potentially concurrent data.
 * Make sure that the following mutexes are locked before calling this function:
 *  - zn->mutex_inner
 */
void __unsafe_zn_add_loc_qle_to_rem_res_map(zn_session_t *zn, _zn_queryable_t *qle)
{
    // Need to check if there is a remote resource declaration matching the new subscription
    zn_reskey_t loc_key;
    loc_key.rid = ZN_RESOURCE_ID_NONE;
    if (qle->key.rid == ZN_RESOURCE_ID_NONE)
        loc_key.rname = qle->key.rname;
    else
        loc_key.rname = __unsafe_zn_get_resource_name_from_key(zn, _ZN_IS_LOCAL, &qle->key);

    _zn_resource_t *rem_res = __unsafe_zn_get_resource_matching_key(zn, _ZN_IS_REMOTE, &loc_key);
    if (rem_res)
    {
        // Update the list of active subscriptions
        z_list_t *qles = z_i_map_get(zn->rem_res_loc_qle_map, rem_res->id);
        qles = z_list_cons(qles, qle);
        z_i_map_set(zn->rem_res_loc_qle_map, rem_res->id, qles);
    }

    if (qle->key.rid != ZN_RESOURCE_ID_NONE)
        free(loc_key.rname);
}

/**
 * This function is unsafe because it operates in potentially concurrent data.
 * Make sure that the following mutexes are locked before calling this function:
 *  - zn->mutex_inner
 */
void __unsafe_zn_add_rem_res_to_loc_qle_map(zn_session_t *zn, z_zint_t id, zn_reskey_t *reskey)
{
    // Check if there is a matching local subscription
    z_list_t *qles = __unsafe_zn_get_queryables_from_remote_key(zn, reskey);
    if (qles)
    {
        // Update the list
        z_list_t *ql = z_i_map_get(zn->rem_res_loc_qle_map, id);
        if (ql)
        {
            // Free any ancient list
            z_list_free(ql);
        }
        // Update the list of active subscriptions
        z_i_map_set(zn->rem_res_loc_qle_map, id, qles);
    }
}

_zn_queryable_t *_zn_get_queryable_by_id(zn_session_t *zn, z_zint_t id)
{
    z_mutex_lock(&zn->mutex_inner);
    _zn_queryable_t *qle = __unsafe_zn_get_queryable_by_id(zn, id);
    z_mutex_unlock(&zn->mutex_inner);
    return qle;
}

int _zn_register_queryable(zn_session_t *zn, _zn_queryable_t *qle)
{
    _Z_DEBUG_VA(">>> Allocating queryable for (%lu,%s,%u)\n", qle->key.rid, qle->key.rname, qle->kind);

    // Acquire the lock on the queryables
    z_mutex_lock(&zn->mutex_inner);

    int res;
    _zn_queryable_t *q = __unsafe_zn_get_queryable_by_id(zn, qle->id);
    if (q)
    {
        // A queryable for this id already exists, return error
        res = -1;
    }
    else
    {
        // Register the queryable
        __unsafe_zn_add_loc_qle_to_rem_res_map(zn, qle);
        zn->local_queryables = z_list_cons(zn->local_queryables, qle);
        res = 0;
    }

    // Release the lock
    z_mutex_unlock(&zn->mutex_inner);

    return res;
}

/**
 * This function is unsafe because it operates in potentially concurrent data.
 * Make sure that the following mutexes are locked before calling this function:
 *  - zn->mutex_inner
 */
void __unsafe_zn_free_queryable(_zn_queryable_t *qle)
{
    _zn_reskey_free(&qle->key);
}

/**
 * This function is unsafe because it operates in potentially concurrent data.
 * Make sure that the following mutexes are locked before calling this function:
 *  - zn->mutex_inner
 */
int __unsafe_zn_queryable_predicate(void *other, void *this)
{
    _zn_queryable_t *o = (_zn_queryable_t *)other;
    _zn_queryable_t *t = (_zn_queryable_t *)this;
    if (t->id == o->id)
    {
        __unsafe_zn_free_queryable(t);
        return 1;
    }
    else
    {
        return 0;
    }
}

void _zn_unregister_queryable(zn_session_t *zn, _zn_queryable_t *qle)
{
    // Acquire the lock on the queryables
    z_mutex_lock(&zn->mutex_inner);

    zn->local_queryables = z_list_remove(zn->local_queryables, __unsafe_zn_queryable_predicate, qle);
    free(qle);

    // Release the lock
    z_mutex_unlock(&zn->mutex_inner);
}

void _zn_flush_queryables(zn_session_t *zn)
{
    // Lock the resources data struct
    z_mutex_lock(&zn->mutex_inner);

    while (zn->local_queryables)
    {
        _zn_queryable_t *qle = (_zn_queryable_t *)z_list_head(zn->local_queryables);
        __unsafe_zn_free_queryable(qle);
        free(qle);
        zn->local_queryables = z_list_pop(zn->local_queryables);
    }
    z_i_map_free(zn->rem_res_loc_qle_map);

    // Release the lock
    z_mutex_unlock(&zn->mutex_inner);
}

void _zn_trigger_queryables(zn_session_t *zn, const _zn_query_t *query)
{
    // Acquire the lock on the queryables
    z_mutex_lock(&zn->mutex_inner);

    // Case 1) -> numeric only reskey
    if (query->key.rname == NULL)
    {
        // Get the declared resource
        _zn_resource_t *res = __unsafe_zn_get_resource_by_id(zn, _ZN_IS_REMOTE, query->key.rid);
        if (res == NULL)
            goto EXIT_QLE_TRIG;

        // Get the complete resource name to be passed to the subscription callback
        z_str_t rname;
        if (res->key.rid == ZN_RESOURCE_ID_NONE)
        {
            // Do not allocate
            rname = res->key.rname;
        }
        else
        {
            // Allocate a computed string
            rname = __unsafe_zn_get_resource_name_from_key(zn, _ZN_IS_LOCAL, &res->key);
            if (rname == NULL)
                goto EXIT_QLE_TRIG;
        }

        // Build the query
        zn_query_t q;
        q.zn = zn;
        q.qid = query->qid;
        q.rname = rname;
        q.predicate = query->predicate;

        // Iterate over the matching queryables
        z_list_t *qles = (z_list_t *)z_i_map_get(zn->rem_res_loc_qle_map, query->key.rid);
        while (qles)
        {
            _zn_queryable_t *qle = (_zn_queryable_t *)z_list_head(qles);
            unsigned int target = (query->target.kind & ZN_QUERYABLE_ALL_KINDS) | (query->target.kind & qle->kind);
            if (target != 0)
            {
                q.kind = qle->kind;
                qle->callback(&q, qle->arg);
            }
            qles = z_list_tail(qles);
        }

        if (res->key.rid != ZN_RESOURCE_ID_NONE)
            free(rname);
    }
    // Case 2) -> string only reskey
    else if (query->key.rid == ZN_RESOURCE_ID_NONE)
    {
        // Build the query
        zn_query_t q;
        q.zn = zn;
        q.qid = query->qid;
        q.rname = query->key.rname;
        q.predicate = query->predicate;

        // Iterate over the matching queryables
        z_list_t *qles = zn->local_queryables;
        while (qles)
        {
            _zn_queryable_t *qle = (_zn_queryable_t *)z_list_head(qles);

            unsigned int target = (query->target.kind & ZN_QUERYABLE_ALL_KINDS) | (query->target.kind & qle->kind);
            if (target != 0)
            {
                // Get the complete resource name to be passed to the subscription callback
                z_str_t rname;
                if (qle->key.rid == ZN_RESOURCE_ID_NONE)
                {
                    // Do not allocate
                    rname = qle->key.rname;
                }
                else
                {
                    // Allocate a computed string
                    rname = __unsafe_zn_get_resource_name_from_key(zn, _ZN_IS_LOCAL, &qle->key);
                    if (rname == NULL)
                        continue;
                }

                if (zn_rname_intersect(rname, query->key.rname))
                {
                    q.kind = qle->kind;
                    qle->callback(&q, qle->arg);
                }

                if (qle->key.rid != ZN_RESOURCE_ID_NONE)
                    free(rname);
            }

            qles = z_list_tail(qles);
        }
    }
    // Case 3) -> numerical reskey with suffix
    else
    {
        // Compute the complete remote resource name starting from the key
        z_str_t rname = __unsafe_zn_get_resource_name_from_key(zn, _ZN_IS_REMOTE, &query->key);
        if (rname == NULL)
            goto EXIT_QLE_TRIG;

        // Build the query
        zn_query_t q;
        q.zn = zn;
        q.qid = query->qid;
        q.rname = query->key.rname;
        q.predicate = query->predicate;

        z_list_t *qles = zn->local_queryables;
        while (qles)
        {
            _zn_queryable_t *qle = (_zn_queryable_t *)z_list_head(qles);

            unsigned int target = (query->target.kind & ZN_QUERYABLE_ALL_KINDS) | (query->target.kind & qle->kind);
            if (target != 0)
            {
                // Get the complete resource name to be passed to the subscription callback
                z_str_t lname;
                if (qle->key.rid == ZN_RESOURCE_ID_NONE)
                {
                    // Do not allocate
                    lname = qle->key.rname;
                }
                else
                {
                    // Allocate a computed string
                    lname = __unsafe_zn_get_resource_name_from_key(zn, _ZN_IS_LOCAL, &qle->key);
                    if (lname == NULL)
                        continue;
                }

                if (zn_rname_intersect(lname, rname))
                {
                    q.kind = qle->kind;
                    qle->callback(&q, qle->arg);
                }

                if (qle->key.rid != ZN_RESOURCE_ID_NONE)
                    free(lname);
            }

            qles = z_list_tail(qles);
        }

        free(rname);
    }

    // Send the final reply
    _zn_zenoh_message_t z_msg = _zn_zenoh_message_init(_ZN_MID_UNIT);
    z_msg.reply_context = _zn_reply_context_init();
    _ZN_SET_FLAG(z_msg.reply_context->header, _ZN_FLAG_Z_F);
    z_msg.reply_context->qid = query->qid;
    z_msg.reply_context->replier_kind = 0;

    if (_zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK) != 0)
    {
        _Z_DEBUG("Trying to reconnect...\n");
        zn->on_disconnect(zn);
        _zn_send_z_msg(zn, &z_msg, zn_reliability_t_RELIABLE, zn_congestion_control_t_BLOCK);
    }

    _zn_zenoh_message_free(&z_msg);

EXIT_QLE_TRIG:
    // Release the lock
    z_mutex_unlock(&zn->mutex_inner);
}
