/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022 Blake W. Felt
 * Copyright (c) 2022-2023 Angus Gratton
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>

#include "py/mpconfig.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/obj.h"
#include "py/objarray.h"
#include "py/objstr.h"
#include "py/runtime.h"

#if MICROPY_HW_ENABLE_USB_RUNTIME_DEVICE

#ifndef NO_QSTR
#include "tusb.h" // TinyUSB is not available when running the string preprocessor
#include "device/dcd.h"
#include "device/usbd.h"
#include "device/usbd_pvt.h"
#include "mp_usbd_internal.h"
#endif

// Maximum number of pending exceptions per single TinyUSB task execution
#define MAX_PEND_EXCS 2

// Top-level singleton object, holds runtime USB device state

typedef struct {
    mp_obj_base_t base;

    // Global callbacks set by USBD.init()
    mp_obj_t descriptor_device_cb;
    mp_obj_t descriptor_config_cb;
    mp_obj_t descriptor_string_cb;
    mp_obj_t open_cb;
    mp_obj_t reset_cb;
    mp_obj_t control_xfer_cb;
    mp_obj_t xfer_cb;

    bool reenumerate; // Pending re-enumerate request

    // Temporary pointers for xfer data in progress on each endpoint
    // Ensuring they aren't garbage collected until the xfer completes
    mp_obj_t xfer_data[CFG_TUD_ENDPPOINT_MAX][2];

    // Pointer to a memoryview that is reused to refer to various pieces of
    // control transfer data that are pushed to USB control transfer
    // callbacks. Python code can't rely on the memoryview contents
    // to remain valid after the callback returns!
    mp_obj_array_t *control_data;

    // Pointers to exceptions thrown inside Python callbacks. See
    // usbd_callback_function_n().
    mp_uint_t num_pend_excs;
    mp_obj_t pend_excs[MAX_PEND_EXCS];
} mp_obj_usbd_t;

const mp_obj_type_t machine_usbd_type;
const mp_obj_type_t mp_type_usbd_static;

// Return a pointer to the data inside a Python buffer provided in a callback
STATIC void *usbd_get_buffer_in_cb(mp_obj_t obj, mp_uint_t flags) {
    mp_buffer_info_t buf_info;
    if (mp_get_buffer(obj, &buf_info, flags)) {
        return buf_info.buf;
    } else {
        mp_obj_t exc = mp_obj_new_exception_msg(&mp_type_TypeError,
            MP_ERROR_TEXT("object with buffer protocol required"));
        mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(exc));
        return NULL;
    }
}

// Call a Python function from inside a TinyUSB callback.
//
// We can't raise any exceptions out of the TinyUSB task, as it may still need
// to do some state cleanup.
//
// The requirement for this becomes very similar to
// mp_call_function_x_protected() for interrupts, but it's more restrictive: if
// the C-based USB-CDC serial port is in use, we can't print from inside a
// TinyUSB callback as it might try to recursively call into TinyUSB to flush
// the CDC port and make room. Therefore, we have to store the exception and
// print it as we exit the TinyUSB task.
//
// (Worse, a single TinyUSB task can process multiple callbacks and therefore generate
// multiple exceptions...)
STATIC mp_obj_t usbd_callback_function_n(mp_obj_t fun, size_t n_args, const mp_obj_t *args) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t ret = mp_call_function_n_kw(fun, n_args, 0, args);
        nlr_pop();
        return ret;
    } else {
        mp_obj_usbd_t *usbd = MP_OBJ_TO_PTR(MP_STATE_VM(usbd));
        assert(usbd != NULL);
        if (usbd->num_pend_excs < MAX_PEND_EXCS) {
            usbd->pend_excs[usbd->num_pend_excs] = MP_OBJ_FROM_PTR(nlr.ret_val);
        }
        usbd->num_pend_excs++;
        return MP_OBJ_NULL;
    }
}

STATIC mp_obj_t usbd_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    (void)type;
    (void)n_args;
    (void)n_kw;
    (void)args;

    if (MP_STATE_VM(usbd) == MP_OBJ_NULL) {
        mp_obj_usbd_t *o = m_new0(mp_obj_usbd_t, 1);
        o->base.type = &machine_usbd_type;
        o->descriptor_device_cb = mp_const_none;
        o->descriptor_config_cb = mp_const_none;
        o->descriptor_string_cb = mp_const_none;
        o->open_cb = mp_const_none;
        o->reset_cb = mp_const_none;
        o->control_xfer_cb = mp_const_none;
        o->xfer_cb = mp_const_none;
        for (int i = 0; i < CFG_TUD_ENDPPOINT_MAX; i++) {
            o->xfer_data[i][0] = mp_const_none;
            o->xfer_data[i][1] = mp_const_none;
        }
        o->reenumerate = false;
        o->control_data = MP_OBJ_TO_PTR(mp_obj_new_memoryview('B', 0, NULL));
        o->num_pend_excs = 0;
        for (int i = 0; i < MAX_PEND_EXCS; i++) {
            o->pend_excs[i] = mp_const_none;
        }

        MP_STATE_VM(usbd) = MP_OBJ_FROM_PTR(o);
    }

    return MP_STATE_VM(usbd);
}

void mp_usbd_deinit(void) {
    // There might be USB transfers in progress right now, so need to stall any live
    // endpoints to prevent the USB stack DMA-ing to/from a buffer which is going away...

    mp_obj_usbd_t *usbd = MP_OBJ_TO_PTR(MP_STATE_VM(usbd));
    if (!usbd) {
        return;
    }
    MP_STATE_VM(usbd) = MP_OBJ_NULL;

    for (int epnum = 0; epnum < CFG_TUD_ENDPPOINT_MAX; epnum++) {
        for (int dir = 0; dir < 2; dir++) {
            if (usbd->xfer_data[epnum][dir] != mp_const_none) {
                usbd_edpt_stall(USBD_RHPORT, tu_edpt_addr(epnum, dir));
            }
        }
    }

    usbd->control_data = NULL;

    // We don't reenumerate at this point as the usbd device is gone. TinyUSB
    // may still send callbacks for the "dynamic" USB endpoints but they will be
    // rejected until usbd is created again.
}

STATIC mp_obj_t usbd_submit_xfer(mp_obj_t self, mp_obj_t ep, mp_obj_t buffer) {
    mp_obj_usbd_t *usbd = (mp_obj_usbd_t *)MP_OBJ_TO_PTR(self);
    int ep_addr;
    mp_buffer_info_t buf_info = { 0 };
    bool result;

    // Unmarshal arguments, raises TypeError if invalid
    ep_addr = mp_obj_get_int(ep);
    mp_get_buffer_raise(buffer, &buf_info, ep_addr & TUSB_DIR_IN_MASK ? MP_BUFFER_READ : MP_BUFFER_RW);

    uint8_t ep_num = tu_edpt_number(ep_addr);
    uint8_t ep_dir = tu_edpt_dir(ep_addr);

    if (ep_num >= CFG_TUD_ENDPPOINT_MAX) {
        // TinyUSB usbd API doesn't range check arguments, so this check avoids
        // out of bounds array access. This C layer doesn't otherwise keep track
        // of which endpoints the host is aware of (or not).
        mp_raise_ValueError("ep");
    }

    if (!usbd_edpt_claim(USBD_RHPORT, ep_addr)) {
        mp_raise_OSError(MP_EBUSY);
    }

    result = usbd_edpt_xfer(USBD_RHPORT, ep_addr, buf_info.buf, buf_info.len);

    if (result) {
        // Store the buffer object until the transfer completes
        usbd->xfer_data[ep_num][ep_dir] = buffer;
    }

    return mp_obj_new_bool(result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(usbd_submit_xfer_obj, usbd_submit_xfer);

STATIC mp_obj_t usbd_reenumerate(mp_obj_t self) {
    mp_obj_usbd_t *usbd = (mp_obj_usbd_t *)MP_OBJ_TO_PTR(self);

    // We may be in a USB-CDC REPL (i.e. inside mp_usbd_task()), so it's not safe to
    // immediately disconnect here.

    // Need to wait until tud_task() exits and do it then. See mp_usbd_task() for
    // implementation.

    usbd->reenumerate = true;

    // Schedule a mp_usbd_task callback in case there isn't one pending
    mp_usbd_schedule_task();

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(usbd_reenumerate_obj, usbd_reenumerate);

STATIC mp_obj_t usbd_stall(size_t n_args, const mp_obj_t *args) {
    // Ignoring args[0] as we don't need to access the usbd instance
    int epnum = mp_obj_get_int(args[1]);

    mp_obj_t res = mp_obj_new_bool(usbd_edpt_stalled(USBD_RHPORT, epnum));

    if (n_args == 3) { // Set stall state
        mp_obj_t stall = args[2];
        if (mp_obj_is_true(stall)) {
            usbd_edpt_stall(USBD_RHPORT, epnum);
        } else {
            usbd_edpt_clear_stall(USBD_RHPORT, epnum);
        }
    }

    return res;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(usbd_stall_obj, 2, 3, usbd_stall);

// Initialize the singleton USB device with all of the relevant transfer and descriptor
// callbacks.
STATIC mp_obj_t usbd_init(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    mp_obj_usbd_t *self = (mp_obj_usbd_t *)MP_OBJ_TO_PTR(pos_args[0]);

    enum { ARG_descriptor_device_cb, ARG_descriptor_config_cb, ARG_descriptor_string_cb, ARG_open_cb,
           ARG_reset_cb, ARG_control_xfer_cb, ARG_xfer_cb };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_descriptor_device_cb, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_descriptor_config_cb, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_descriptor_string_cb, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_open_cb, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_reset_cb, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_control_xfer_cb, MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_xfer_cb, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    self->descriptor_device_cb = args[ARG_descriptor_device_cb].u_obj;
    self->descriptor_config_cb = args[ARG_descriptor_config_cb].u_obj;
    self->descriptor_string_cb = args[ARG_descriptor_string_cb].u_obj;
    self->open_cb = args[ARG_open_cb].u_obj;
    self->reset_cb = args[ARG_reset_cb].u_obj;
    self->control_xfer_cb = args[ARG_control_xfer_cb].u_obj;
    self->xfer_cb = args[ARG_xfer_cb].u_obj;

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(usbd_init_obj, 1, usbd_init);

// usbd_static Python object is a wrapper for the static properties of the USB device
// (i.e. values used by the C implementation of TinyUSB devices.)
STATIC const MP_DEFINE_BYTES_OBJ(desc_device_obj,
    &mp_usbd_desc_device_static, sizeof(tusb_desc_device_t));
STATIC const MP_DEFINE_BYTES_OBJ(desc_cfg_obj,
    mp_usbd_desc_cfg_static, USBD_STATIC_DESC_LEN);

STATIC const mp_rom_map_elem_t usbd_static_properties_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_itf_max), MP_OBJ_NEW_SMALL_INT(USBD_ITF_STATIC_MAX) },
    { MP_ROM_QSTR(MP_QSTR_ep_max), MP_OBJ_NEW_SMALL_INT(USBD_EP_STATIC_MAX) },
    { MP_ROM_QSTR(MP_QSTR_str_max), MP_OBJ_NEW_SMALL_INT(USBD_STR_STATIC_MAX) },
    { MP_ROM_QSTR(MP_QSTR_desc_device), MP_ROM_PTR(&desc_device_obj)  },
    { MP_ROM_QSTR(MP_QSTR_desc_cfg), MP_ROM_PTR(&desc_cfg_obj) },
};
STATIC MP_DEFINE_CONST_DICT(usbd_static_properties_dict, usbd_static_properties_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_usbd_static,
    MP_QSTR_usbd_static,
    MP_TYPE_FLAG_NONE,
    locals_dict, &usbd_static_properties_dict
    );

STATIC const mp_rom_map_elem_t usbd_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_static), MP_ROM_PTR(&mp_type_usbd_static) },
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&usbd_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_submit_xfer), MP_ROM_PTR(&usbd_submit_xfer_obj) },
    { MP_ROM_QSTR(MP_QSTR_reenumerate), MP_ROM_PTR(&usbd_reenumerate_obj) },
    { MP_ROM_QSTR(MP_QSTR_stall), MP_ROM_PTR(&usbd_stall_obj) },
};
STATIC MP_DEFINE_CONST_DICT(usbd_locals_dict, usbd_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    machine_usbd_type,
    MP_QSTR_USBD,
    MP_TYPE_FLAG_NONE,
    make_new, usbd_make_new,
    locals_dict, &usbd_locals_dict
    );

MP_REGISTER_ROOT_POINTER(mp_obj_t usbd);

// Common code path for descriptor callback functions that read descriptor
// values back from the relevant Python callback, or fall back to the static
// result if no callback is set.
STATIC const uint8_t *_usbd_handle_descriptor_cb(mp_obj_t callback, const void *static_result) {
    mp_obj_usbd_t *usbd = MP_OBJ_TO_PTR(MP_STATE_VM(usbd));

    if (usbd == NULL || callback == mp_const_none) {
        // The USBD object isn't initialized or this callback is unset,
        // so return the static descriptor
        return static_result;
    }

    mp_obj_t cb_res = usbd_callback_function_n(callback, 0, NULL);
    const void *desc_res;

    if (cb_res == MP_OBJ_NULL) {
        // Exception occurred in callback
        cb_res = mp_const_none;
        desc_res = static_result;
    } else {
        // If the callback returned a non-buffer object then this will
        // queue an exception for later and return the static descriptor.
        desc_res = usbd_get_buffer_in_cb(cb_res, MP_BUFFER_READ);
        if (desc_res == NULL) {
            desc_res = static_result;
        }
    }

    usbd->xfer_data[0][TUSB_DIR_IN] = cb_res;
    return desc_res;
}

const uint8_t *tud_descriptor_device_cb(void) {
    mp_obj_usbd_t *usbd = MP_OBJ_TO_PTR(MP_STATE_VM(usbd));
    return _usbd_handle_descriptor_cb(usbd ? usbd->descriptor_device_cb : mp_const_none,
        &mp_usbd_desc_device_static);
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    mp_obj_usbd_t *usbd = MP_OBJ_TO_PTR(MP_STATE_VM(usbd));
    return _usbd_handle_descriptor_cb(usbd ? usbd->descriptor_config_cb : mp_const_none,
        mp_usbd_desc_cfg_static);
}

const char *mp_usbd_internal_dynamic_descriptor_string_cb(uint8_t index) {
    mp_obj_usbd_t *usbd = MP_OBJ_TO_PTR(MP_STATE_VM(usbd));

    if (usbd == NULL || usbd->descriptor_string_cb == mp_const_none) {
        return NULL;
    }

    mp_obj_t args[] = { mp_obj_new_int(index) };
    mp_obj_t callback_res = usbd_callback_function_n(usbd->descriptor_string_cb, 1, args);

    if (callback_res == mp_const_none || callback_res == MP_OBJ_NULL) {
        return NULL;
    }
    return usbd_get_buffer_in_cb(callback_res, MP_BUFFER_READ);
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    return false; // Currently no support for Vendor control transfers on the Python side
}

// Generic "runtime device" TinyUSB class driver, delegates everything to Python callbacks

STATIC void runtime_dev_init(void) {
}

STATIC void runtime_dev_reset(uint8_t rhport) {
    mp_obj_usbd_t *usbd = MP_OBJ_TO_PTR(MP_STATE_VM(usbd));
    if (!usbd) {
        return;
    }

    for (int epnum = 0; epnum < CFG_TUD_ENDPPOINT_MAX; epnum++) {
        for (int dir = 0; dir < 2; dir++) {
            usbd->xfer_data[epnum][dir] = mp_const_none;
        }
    }

    if (mp_obj_is_callable(usbd->reset_cb)) {
        usbd_callback_function_n(usbd->reset_cb, 0, NULL);
    }
}

STATIC uint16_t runtime_dev_open(uint8_t rhport, tusb_desc_interface_t const *itf_desc, uint16_t max_len) {
    mp_obj_usbd_t *usbd = MP_OBJ_TO_PTR(MP_STATE_VM(usbd));
    const uint8_t *p_desc = (const void *)itf_desc;
    uint16_t claim_len = 0;

    if (!usbd) {
        return 0;
    }

    // Claim any interfaces (and associated descriptor data) that aren't in the interface number range reserved for
    // static drivers.
    while (claim_len < max_len && (tu_desc_type(p_desc) != TUSB_DESC_INTERFACE ||
                                   ((tusb_desc_interface_t *)p_desc)->bInterfaceNumber >= USBD_ITF_STATIC_MAX)) {
        if (tu_desc_type(p_desc) == TUSB_DESC_ENDPOINT) {
            // Open all the endpoints found in the descriptor
            bool r = usbd_edpt_open(USBD_RHPORT, (const void *)p_desc);
            if (!r) {
                mp_obj_t exc = mp_obj_new_exception_arg1(&mp_type_OSError, MP_OBJ_NEW_SMALL_INT(MP_ENODEV));
                mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(exc));
                break;
            }
        }

        claim_len += tu_desc_len(p_desc);
        p_desc += tu_desc_len(p_desc);
    }

    if (claim_len && mp_obj_is_callable(usbd->open_cb)) {
        // Repurpose the control_data memoryview to point into itf_desc for this one call
        usbd->control_data->items = (void *)itf_desc;
        usbd->control_data->len = claim_len;
        mp_obj_t args[] = { MP_OBJ_FROM_PTR(usbd->control_data) };
        usbd_callback_function_n(usbd->open_cb, 1, args);
        usbd->control_data->len = 0;
        usbd->control_data->items = NULL;
    }

    return claim_len;
}

STATIC bool runtime_dev_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    mp_obj_t cb_res = mp_const_false;
    mp_obj_usbd_t *usbd = MP_OBJ_TO_PTR(MP_STATE_VM(usbd));
    tusb_dir_t dir = request->bmRequestType_bit.direction;
    mp_buffer_info_t buf_info;

    if (!usbd) {
        return false;
    }

    if (mp_obj_is_callable(usbd->control_xfer_cb)) {
        usbd->control_data->items = (void *)request;
        usbd->control_data->len = sizeof(tusb_control_request_t);
        mp_obj_t args[] = {
            mp_obj_new_int(stage),
            MP_OBJ_FROM_PTR(usbd->control_data),
        };
        cb_res = usbd_callback_function_n(usbd->control_xfer_cb, MP_ARRAY_SIZE(args), args);
        usbd->control_data->items = NULL;
        usbd->control_data->len = 0;

        if (cb_res == MP_OBJ_NULL) {
            // Exception occurred in the callback handler, stall this transfer
            cb_res = mp_const_false;
        }
    }

    // Check if callback returned any data to submit
    if (mp_get_buffer(cb_res, &buf_info, dir == TUSB_DIR_IN ? MP_BUFFER_READ : MP_BUFFER_RW)) {
        bool result = tud_control_xfer(USBD_RHPORT,
            request,
            buf_info.buf,
            buf_info.len);

        if (result) {
            // Keep buffer object alive until the transfer completes
            usbd->xfer_data[0][dir] = cb_res;
        }

        return result;
    } else {
        // Expect True or False to stall or continue

        if (stage == CONTROL_STAGE_ACK) {
            // Allow data to be GCed once it's no longer in use
            usbd->xfer_data[0][dir] = mp_const_none;
        }
        return mp_obj_is_true(cb_res);
    }
}

STATIC bool runtime_dev_xfer_cb(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
    mp_obj_t ep = mp_obj_new_int(ep_addr);
    mp_obj_t cb_res = mp_const_false;
    mp_obj_usbd_t *usbd = MP_OBJ_TO_PTR(MP_STATE_VM(usbd));
    if (!usbd) {
        return false;
    }

    if (mp_obj_is_callable(usbd->xfer_cb)) {
        mp_obj_t args[] = {
            ep,
            MP_OBJ_NEW_SMALL_INT(result),
            MP_OBJ_NEW_SMALL_INT(xferred_bytes),
        };
        cb_res = usbd_callback_function_n(usbd->xfer_cb, MP_ARRAY_SIZE(args), args);
    }

    // Clear any xfer_data for this endpoint
    usbd->xfer_data[tu_edpt_number(ep_addr)][tu_edpt_dir(ep_addr)] = mp_const_none;

    return cb_res != MP_OBJ_NULL && mp_obj_is_true(cb_res);
}

STATIC usbd_class_driver_t const _runtime_dev_driver =
{
    #if CFG_TUSB_DEBUG >= 2
    .name = "runtime_dev",
    #endif
    .init = runtime_dev_init,
    .reset = runtime_dev_reset,
    .open = runtime_dev_open,
    .control_xfer_cb = runtime_dev_control_xfer_cb,
    .xfer_cb = runtime_dev_xfer_cb,
    .sof = NULL
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &_runtime_dev_driver;
}

void mp_usbd_task(void) {
    static bool in_task;
    if (in_task) {
        // If this exception triggers, it means a USB callback tried to do
        // something that itself became blocked on TinyUSB (most likely: read or
        // write from a C-based USB-CDC serial port.)
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("TinyUSB callback can't recurse"));
    }
    in_task = true;
    tud_task_ext(0, false);

    mp_obj_usbd_t *usbd = MP_OBJ_TO_PTR(MP_STATE_VM(usbd));
    if (usbd && usbd->reenumerate) {
        // This should reconfigure the USB peripheral so the host no longer sees the device
        tud_disconnect();

        // Turns out this is the most reliable way to ensure the host gives up
        // on the device up on the device. The host should register that the
        // device is "gone" during this time, and will then try to enumerate
        // again when we reconnect.
        mp_hal_delay_ms(50);

        tud_connect();

        usbd->reenumerate = false;
    }

    in_task = false;

    if (usbd) {
        // Print any exceptions that were raised by Python callbacks
        // inside tud_task_ext(). See usbd_callback_function_n.

        // As printing exceptions to USB-CDC may recursively call mp_usbd_task(),
        // first copy out the pending data to the local stack
        mp_uint_t num_pend_excs = usbd->num_pend_excs;
        mp_obj_t pend_excs[MAX_PEND_EXCS];
        for (mp_uint_t i = 0; i < MIN(MAX_PEND_EXCS, num_pend_excs); i++) {
            pend_excs[i] = usbd->pend_excs[i];
            usbd->pend_excs[i] = mp_const_none;
        }
        usbd->num_pend_excs = 0;

        // Now print the exceptions stored from this mp_usbd_task() call
        for (mp_uint_t i = 0; i < MIN(MAX_PEND_EXCS, num_pend_excs); i++) {
            mp_obj_print_exception(&mp_plat_print, pend_excs[i]);
        }
        if (num_pend_excs > MAX_PEND_EXCS) {
            mp_printf(&mp_plat_print, "%u additional exceptions in USB callbacks\n",
                num_pend_excs - MAX_PEND_EXCS);
        }
    }
}

#endif // MICROPY_HW_ENABLE_USB_RUNTIME_DEVICE
