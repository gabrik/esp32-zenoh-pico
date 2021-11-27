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

#include "zenoh-pico/protocol/private/utils.h"
#include "zenoh-pico/utils/private/logging.h"
#include "zenoh-pico/system/common.h"
#include "zenoh-pico/transport/private/utils.h"

/*------------------ SN helper ------------------*/
/**
 * This function is unsafe because it operates in potentially concurrent data.
 * Make sure that the following mutexes are locked before calling this function:
 *  - zn->mutex_inner
 */
z_zint_t __unsafe_zn_get_sn(zn_session_t *zn, zn_reliability_t reliability)
{
    z_zint_t sn;
    // Get the sequence number and update it in modulo operation
    if (reliability == zn_reliability_t_RELIABLE)
    {
        sn = zn->sn_tx_reliable;
        zn->sn_tx_reliable = (zn->sn_tx_reliable + 1) % zn->sn_resolution;
    }
    else
    {
        sn = zn->sn_tx_best_effort;
        zn->sn_tx_best_effort = (zn->sn_tx_best_effort + 1) % zn->sn_resolution;
    }
    return sn;
}

int _zn_sn_precedes(z_zint_t sn_resolution_half, z_zint_t sn_left, z_zint_t sn_right)
{
    if (sn_right > sn_left)
        return (sn_right - sn_left <= sn_resolution_half);
    else
        return (sn_left - sn_right > sn_resolution_half);
}

/*------------------ Transmission helper ------------------*/
/**
 * This function is unsafe because it operates in potentially concurrent data.
 * Make sure that the following mutexes are locked before calling this function:
 *  - zn->mutex_tx
 */
void __unsafe_zn_prepare_wbuf(_z_wbuf_t *buf, int is_streamed)
{
    _z_wbuf_clear(buf);

    if (is_streamed == 1)
    {
        // NOTE: 16 bits (2 bytes) may be prepended to the serialized message indicating the total length
        //       in bytes of the message, resulting in the maximum length of a message being 65_535 bytes.
        //       This is necessary in those stream-oriented transports (e.g., TCP) that do not preserve
        //       the boundary of the serialized messages. The length is encoded as little-endian.
        //       In any case, the length of a message must not exceed 65_535 bytes.
        for (size_t i = 0; i < _ZN_MSG_LEN_ENC_SIZE; ++i)
            _z_wbuf_put(buf, 0, i);
        _z_wbuf_set_wpos(buf, _ZN_MSG_LEN_ENC_SIZE);
    }
}

/**
 * This function is unsafe because it operates in potentially concurrent data.
 * Make sure that the following mutexes are locked before calling this function:
 *  - zn->mutex_tx
 */
void __unsafe_zn_finalize_wbuf(_z_wbuf_t *buf, int is_streamed)
{
    if (is_streamed == 1)
    {
        // NOTE: 16 bits (2 bytes) may be prepended to the serialized message indicating the total length
        //       in bytes of the message, resulting in the maximum length of a message being 65_535 bytes.
        //       This is necessary in those stream-oriented transports (e.g., TCP) that do not preserve
        //       the boundary of the serialized messages. The length is encoded as little-endian.
        //       In any case, the length of a message must not exceed 65_535 bytes.
        size_t len = _z_wbuf_len(buf) - _ZN_MSG_LEN_ENC_SIZE;
        for (size_t i = 0; i < _ZN_MSG_LEN_ENC_SIZE; ++i)
            _z_wbuf_put(buf, (uint8_t)((len >> 8 * i) & 0xFF), i);
    }
}

int _zn_send_t_msg(zn_session_t *zn, _zn_transport_message_t *t_msg)
{
    _Z_DEBUG(">> send session message\n");

    // Acquire the lock
    z_mutex_lock(&zn->mutex_tx);

    // Prepare the buffer eventually reserving space for the message length
    __unsafe_zn_prepare_wbuf(&zn->wbuf, zn->link->is_streamed);

    // Encode the session message
    int res = _zn_transport_message_encode(&zn->wbuf, t_msg);
    if (res == 0)
    {
        // Write the message legnth in the reserved space if needed
        __unsafe_zn_finalize_wbuf(&zn->wbuf, zn->link->is_streamed);
        // Send the wbuf on the socket
        res = _zn_send_wbuf(zn->link, &zn->wbuf);
        // Mark the session that we have transmitted data
        zn->transmitted = 1;
    }
    else
    {
        _Z_DEBUG("Dropping session message because it is too large");
    }

    // Release the lock
    z_mutex_unlock(&zn->mutex_tx);

    return res;
}

_zn_transport_message_t __zn_frame_header(zn_reliability_t reliability, int is_fragment, int is_final, z_zint_t sn)
{
    // Create the frame session message that carries the zenoh message
    _zn_transport_message_t t_msg = _zn_transport_message_init(_ZN_MID_FRAME);
    t_msg.body.frame.sn = sn;

    if (reliability == zn_reliability_t_RELIABLE)
        _ZN_SET_FLAG(t_msg.header, _ZN_FLAG_T_R);

    if (is_fragment)
    {
        _ZN_SET_FLAG(t_msg.header, _ZN_FLAG_T_F);

        if (is_final)
            _ZN_SET_FLAG(t_msg.header, _ZN_FLAG_T_E);

        // Do not add the payload
        t_msg.body.frame.payload.fragment.len = 0;
        t_msg.body.frame.payload.fragment.val = NULL;
    }
    else
    {
        // Do not allocate the vector containing the messages
        t_msg.body.frame.payload.messages._capacity = 0;
        t_msg.body.frame.payload.messages._len = 0;
        t_msg.body.frame.payload.messages._val = NULL;
    }

    return t_msg;
}

/**
 * This function is unsafe because it operates in potentially concurrent data.
 * Make sure that the following mutexes are locked before calling this function:
 *  - zn->mutex_tx
 */
int __unsafe_zn_serialize_zenoh_fragment(_z_wbuf_t *dst, _z_wbuf_t *src, zn_reliability_t reliability, size_t sn)
{
    // Assume first that this is not the final fragment
    int is_final = 0;
    do
    {
        // Mark the buffer for the writing operation
        size_t w_pos = _z_wbuf_get_wpos(dst);
        // Get the frame header
        _zn_transport_message_t f_hdr = __zn_frame_header(reliability, 1, is_final, sn);
        // Encode the frame header
        int res = _zn_transport_message_encode(dst, &f_hdr);
        if (res == 0)
        {
            size_t space_left = _z_wbuf_space_left(dst);
            size_t bytes_left = _z_wbuf_len(src);
            // Check if it is really the final fragment
            if (!is_final && (bytes_left <= space_left))
            {
                // Revert the buffer
                _z_wbuf_set_wpos(dst, w_pos);
                // It is really the finally fragment, reserialize the header
                is_final = 1;
                continue;
            }
            // Write the fragment
            size_t to_copy = bytes_left <= space_left ? bytes_left : space_left;
            return _z_wbuf_copy_into(dst, src, to_copy);
        }
        else
        {
            return 0;
        }
    } while (1);
}

int _zn_send_z_msg(zn_session_t *zn, _zn_zenoh_message_t *z_msg, zn_reliability_t reliability, zn_congestion_control_t cong_ctrl)
{
    _Z_DEBUG(">> send zenoh message\n");

    // Acquire the lock and drop the message if needed
    if (cong_ctrl == zn_congestion_control_t_BLOCK)
    {
        z_mutex_lock(&zn->mutex_tx);
    }
    else
    {
        int locked = z_mutex_trylock(&zn->mutex_tx);
        if (locked != 0)
        {
            _Z_DEBUG("Dropping zenoh message because of congestion control\n");
            // We failed to acquire the lock, drop the message
            return 0;
        }
    }

    // Prepare the buffer eventually reserving space for the message length
    __unsafe_zn_prepare_wbuf(&zn->wbuf, zn->link->is_streamed);

    // Get the next sequence number
    z_zint_t sn = __unsafe_zn_get_sn(zn, reliability);
    // Create the frame header that carries the zenoh message
    _zn_transport_message_t t_msg = __zn_frame_header(reliability, 0, 0, sn);

    // Encode the frame header
    int res = _zn_transport_message_encode(&zn->wbuf, &t_msg);
    if (res != 0)
    {
        _Z_DEBUG("Dropping zenoh message because the session frame can not be encoded\n");
        goto EXIT_ZSND_PROC;
    }

    // Encode the zenoh message
    res = _zn_zenoh_message_encode(&zn->wbuf, z_msg);
    if (res == 0)
    {
        // Write the message legnth in the reserved space if needed
        __unsafe_zn_finalize_wbuf(&zn->wbuf, zn->link->is_streamed);

        // Send the wbuf on the socket
        res = _zn_send_wbuf(zn->link, &zn->wbuf);
        if (res == 0)
            // Mark the session that we have transmitted data
            zn->transmitted = 1;
    }
    else
    {
        // The message does not fit in the current batch, let's fragment it
        // Create an expandable wbuf for fragmentation
        _z_wbuf_t fbf = _z_wbuf_make(ZN_FRAG_BUF_TX_CHUNK, 1);

        // Encode the message on the expandable wbuf
        res = _zn_zenoh_message_encode(&fbf, z_msg);
        if (res != 0)
        {
            _Z_DEBUG("Dropping zenoh message because it can not be fragmented");
            goto EXIT_FRAG_PROC;
        }

        // Fragment and send the message
        int is_first = 1;
        while (_z_wbuf_len(&fbf) > 0)
        {
            // Get the fragment sequence number
            if (!is_first)
                sn = __unsafe_zn_get_sn(zn, reliability);
            is_first = 0;

            // Clear the buffer for serialization
            __unsafe_zn_prepare_wbuf(&zn->wbuf, zn->link->is_streamed);

            // Serialize one fragment
            res = __unsafe_zn_serialize_zenoh_fragment(&zn->wbuf, &fbf, reliability, sn);
            if (res != 0)
            {
                _Z_DEBUG("Dropping zenoh message because it can not be fragmented\n");
                goto EXIT_FRAG_PROC;
            }

            // Write the message length in the reserved space if needed
            __unsafe_zn_finalize_wbuf(&zn->wbuf, zn->link->is_streamed);

            // Send the wbuf on the socket
            res = _zn_send_wbuf(zn->link, &zn->wbuf);
            if (res != 0)
            {
                _Z_DEBUG("Dropping zenoh message because it can not sent\n");
                goto EXIT_FRAG_PROC;
            }

            // Mark the session that we have transmitted data
            zn->transmitted = 1;
        }

    EXIT_FRAG_PROC:
        // Free the fragmentation buffer memory
        _z_wbuf_free(&fbf);
    }

EXIT_ZSND_PROC:
    // Release the lock
    z_mutex_unlock(&zn->mutex_tx);

    return res;
}
