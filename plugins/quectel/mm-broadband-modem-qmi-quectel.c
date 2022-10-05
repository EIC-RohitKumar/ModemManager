/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details:
 *
 * Copyright (C) 2018 Aleksander Morgado <aleksander@aleksander.es>
 */

#include <config.h>

#include "mm-broadband-modem-qmi-quectel.h"
#include "mm-shared-quectel.h"
#include "mm-iface-modem-firmware.h"
#include "mm-iface-modem-3gpp.h"
#include "mm-iface-modem-voice.h"
#include "mm-base-modem-at.h"
#include "mm-broadband-bearer.h"
#include "mm-log.h"

static void iface_modem_init          (MMIfaceModem         *iface);
static void shared_quectel_init       (MMSharedQuectel      *iface);
static void iface_modem_firmware_init (MMIfaceModemFirmware *iface);
static void iface_modem_3gpp_init     (MMIfaceModem3gpp     *iface);

static MMIfaceModem3gpp  *iface_modem_3gpp_parent;

G_DEFINE_TYPE_EXTENDED (MMBroadbandModemQmiQuectel, mm_broadband_modem_qmi_quectel, MM_TYPE_BROADBAND_MODEM_QMI, 0,
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM, iface_modem_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_FIRMWARE, iface_modem_firmware_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_SHARED_QUECTEL, shared_quectel_init)
                        G_IMPLEMENT_INTERFACE (MM_TYPE_IFACE_MODEM_3GPP, iface_modem_3gpp_init))


/*****************************************************************************/
/* Set initial EPS bearer */

typedef enum {
    SET_INITIAL_EPS_STEP_FIRST = 0,
    SET_INITIAL_EPS_STEP_CHECK_MODE,
    SET_INITIAL_EPS_STEP_RF_OFF,
    SET_INITIAL_EPS_STEP_SET_APN,
    SET_INITIAL_EPS_STEP_AUTH,
    SET_INITIAL_EPS_STEP_RF_ON,
    SET_INITIAL_EPS_STEP_LAST,
} SetInitialEpsStep;

typedef struct {
    MMBearerProperties *properties;
    SetInitialEpsStep   step;
    guint               initial_cfun_mode;
    GError             *saved_error;
} SetInitialEpsContext;

static void
set_initial_eps_context_free (SetInitialEpsContext *ctx)
{
    g_assert (!ctx->saved_error);
    g_object_unref (ctx->properties);
    g_slice_free (SetInitialEpsContext, ctx);
}

static gboolean
modem_3gpp_set_initial_eps_bearer_settings_finish (MMIfaceModem3gpp  *self,
                                                   GAsyncResult      *res,
                                                   GError           **error)
{
    return g_task_propagate_boolean (G_TASK (res), error);
}

static void set_initial_eps_step (GTask *task);

static void
set_initial_eps_rf_on_ready (MMBaseModem  *self,
                             GAsyncResult *res,
                             GTask        *task)
{
    g_autoptr(GError)     error = NULL;
    SetInitialEpsContext *ctx;

    ctx = (SetInitialEpsContext *) g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (self, res, &error)) {
        mm_obj_warn (self, "couldn't set RF back on: %s", error->message);
        if (!ctx->saved_error)
            ctx->saved_error = g_steal_pointer (&error);
    }

    /* Go to next step */
    ctx->step++;
    set_initial_eps_step (task);
}

static void
set_initial_eps_rf_off_ready (MMBaseModem  *self,
                              GAsyncResult *res,
                              GTask        *task)
{
    GError               *error = NULL;
    SetInitialEpsContext *ctx;

    ctx = (SetInitialEpsContext *) g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (self, res, &error)) {
        mm_obj_warn (self, "couldn't set RF off: %s", error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    /* Go to next step */
    ctx->step++;
    set_initial_eps_step (task);
}

static void
set_initial_eps_cgdcont_ready (MMBaseModem  *_self,
                               GAsyncResult *res,
                               GTask        *task)
{
    MMBroadbandModemQmiQuectel  *self = MM_BROADBAND_MODEM_QMI_QUECTEL (_self);
    SetInitialEpsContext        *ctx;

    ctx = (SetInitialEpsContext *) g_task_get_task_data (task);

    if (!mm_base_modem_at_command_finish (_self, res, &ctx->saved_error)) {
        mm_obj_warn (self, "couldn't configure context %d settings: %s",
                     self->initial_eps_bearer_cid, ctx->saved_error->message);
        /* Fallback to recover RF before returning the error  */
        ctx->step = SET_INITIAL_EPS_STEP_RF_ON;
    } else {
        /* Go to next step */
        ctx->step++;
    }
    set_initial_eps_step (task);
}

static void
set_initial_eps_cfun_mode_load_ready (MMBaseModem  *self,
                                      GAsyncResult *res,
                                      GTask        *task)
{
    GError                *error = NULL;
    const gchar           *response;
    g_autoptr(GRegex)      r = NULL;
    g_autoptr(GMatchInfo)  match_info = NULL;
    SetInitialEpsContext  *ctx;
    guint                  mode;

    ctx = (SetInitialEpsContext *) g_task_get_task_data (task);
    response = mm_base_modem_at_command_finish (self, res, &error);
    if (!response || !mm_3gpp_parse_cfun_query_response (response, &mode, &error)) {
        mm_obj_warn (self, "couldn't load initial functionality mode: %s", error->message);
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    mm_obj_dbg (self, "current functionality mode: %u", mode);
    if (mode != 1 && mode != 4) {
        g_task_return_new_error (task, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
                                 "cannot setup the default LTE bearer settings: "
                                 "the SIM must be powered");
        g_task_return_error (task, error);
        g_object_unref (task);
        return;
    }

    ctx->initial_cfun_mode = mode;
    ctx->step++;
    set_initial_eps_step (task);
}

static void
set_initial_eps_step (GTask *task)
{
    MMBroadbandModemQmiQuectel  *self;
    SetInitialEpsContext        *ctx;

    self = g_task_get_source_object (task);
    ctx  = g_task_get_task_data (task);

    switch (ctx->step) {
    case SET_INITIAL_EPS_STEP_FIRST:
        ctx->step++;
        /* fall through */

    case SET_INITIAL_EPS_STEP_CHECK_MODE:
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            "+CFUN?",
            5,
            FALSE,
            (GAsyncReadyCallback)set_initial_eps_cfun_mode_load_ready,
            task);
        return;

    case SET_INITIAL_EPS_STEP_RF_OFF:
        if (ctx->initial_cfun_mode != 4) {
            mm_base_modem_at_command (
                MM_BASE_MODEM (self),
                "+CFUN=4",
                5,
                FALSE,
                (GAsyncReadyCallback)set_initial_eps_rf_off_ready,
                task);
            return;
        }
        ctx->step++;
        /* fall through */

    case SET_INITIAL_EPS_STEP_SET_APN:  {
        const gchar        *apn;
        g_autofree gchar   *quoted_apn = NULL;
        g_autofree gchar   *apn_cmd = NULL;
        const gchar        *ip_family_str;
        MMBearerIpFamily    ip_family;

        /* For Quectel modems BG-96 & EG-95 we are using default cid value = 1
           This value can be different for other Quectel modems */
        self->initial_eps_bearer_cid = 1;

        ip_family = mm_bearer_properties_get_ip_type (ctx->properties);
        if (ip_family == MM_BEARER_IP_FAMILY_NONE || ip_family == MM_BEARER_IP_FAMILY_ANY)
            ip_family = MM_BEARER_IP_FAMILY_IPV4;
        ip_family_str = mm_3gpp_get_pdp_type_from_ip_family (ip_family);
        apn = mm_bearer_properties_get_apn (ctx->properties);
        mm_obj_dbg (self, "context %d with APN '%s' and PDP type '%s'",
                                   self->initial_eps_bearer_cid, apn, ip_family_str);
        quoted_apn = mm_port_serial_at_quote_string (apn);
        apn_cmd = g_strdup_printf ("+CGDCONT=%u,\"%s\",%s",
                                   self->initial_eps_bearer_cid, ip_family_str, quoted_apn);
        mm_base_modem_at_command (
            MM_BASE_MODEM (self),
            apn_cmd,
            20,
            FALSE,
            (GAsyncReadyCallback)set_initial_eps_cgdcont_ready,
            task);

        return;
    }

    case SET_INITIAL_EPS_STEP_AUTH:
        /* No EPS bearer authentication method is provided for Quectel modems
           so skipping this part */
        ctx->step++;
        /* fall through */

    case SET_INITIAL_EPS_STEP_RF_ON:
        if (ctx->initial_cfun_mode == 1) {
            mm_base_modem_at_command (
                MM_BASE_MODEM (self),
                "+CFUN=1",
                5,
                FALSE,
                (GAsyncReadyCallback)set_initial_eps_rf_on_ready,
                task);
            return;
        }
        ctx->step++;
        /* fall through */

    case SET_INITIAL_EPS_STEP_LAST:
        if (ctx->saved_error)
            g_task_return_error (task, g_steal_pointer (&ctx->saved_error));
        else
            g_task_return_boolean (task, TRUE);
        g_object_unref (task);
        return;

    default:
        g_assert_not_reached ();
    }
}

static void
modem_3gpp_set_initial_eps_bearer_settings (MMIfaceModem3gpp    *self,
                                            MMBearerProperties  *properties,
                                            GAsyncReadyCallback  callback,
                                            gpointer             user_data)
{
    GTask                *task;
    SetInitialEpsContext *ctx;

    task = g_task_new (self, NULL, callback, user_data);

    /* Setup context */
    ctx = g_slice_new0 (SetInitialEpsContext);
    ctx->properties = g_object_ref (properties);
    ctx->step = SET_INITIAL_EPS_STEP_FIRST;
    g_task_set_task_data (task, ctx, (GDestroyNotify) set_initial_eps_context_free);

    set_initial_eps_step (task);
}

/*****************************************************************************/

MMBroadbandModemQmiQuectel *
mm_broadband_modem_qmi_quectel_new (const gchar  *device,
                                    const gchar **drivers,
                                    const gchar  *plugin,
                                    guint16       vendor_id,
                                    guint16       product_id)
{
    return g_object_new (MM_TYPE_BROADBAND_MODEM_QMI_QUECTEL,
                         MM_BASE_MODEM_DEVICE, device,
                         MM_BASE_MODEM_DRIVERS, drivers,
                         MM_BASE_MODEM_PLUGIN, plugin,
                         MM_BASE_MODEM_VENDOR_ID, vendor_id,
                         MM_BASE_MODEM_PRODUCT_ID, product_id,
                         NULL);
}

static void
mm_broadband_modem_qmi_quectel_init (MMBroadbandModemQmiQuectel *self)
{
}

static void
iface_modem_init (MMIfaceModem *iface)
{
    iface->setup_sim_hot_swap = mm_shared_quectel_setup_sim_hot_swap;
    iface->setup_sim_hot_swap_finish = mm_shared_quectel_setup_sim_hot_swap_finish;
}

static void
iface_modem_firmware_init (MMIfaceModemFirmware *iface)
{
    iface->load_update_settings = mm_shared_quectel_firmware_load_update_settings;
    iface->load_update_settings_finish = mm_shared_quectel_firmware_load_update_settings_finish;
}

static void
shared_quectel_init (MMSharedQuectel *iface)
{
}

static void
mm_broadband_modem_qmi_quectel_class_init (MMBroadbandModemQmiQuectelClass *klass)
{
}

static void
iface_modem_3gpp_init (MMIfaceModem3gpp *iface)
{
    iface_modem_3gpp_parent = g_type_interface_peek_parent (iface);

    iface->set_initial_eps_bearer_settings = modem_3gpp_set_initial_eps_bearer_settings;
    iface->set_initial_eps_bearer_settings_finish = modem_3gpp_set_initial_eps_bearer_settings_finish;
}
