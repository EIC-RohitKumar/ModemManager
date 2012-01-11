/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * mmcli -- Control modem status & access information from the command line
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2011 Aleksander Morgado <aleksander@gnu.org>
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include <libmm-glib.h>

#include "mmcli.h"
#include "mmcli-common.h"

/* Context */
typedef struct {
    MMManager *manager;
    GCancellable *cancellable;
    MMObject *object;
    MMModem3gpp *modem_3gpp;
} Context;
static Context *ctx;

/* Options */
static gboolean scan_flag;
static gboolean register_home_flag;
static gchar *register_in_operator_str;

static GOptionEntry entries[] = {
    { "3gpp-scan", 0, 0, G_OPTION_ARG_NONE, &scan_flag,
      "Scan for available networks in a given modem.",
      NULL
    },
    { "3gpp-register-home", 0, 0, G_OPTION_ARG_NONE, &register_home_flag,
      "Request a given modem to register in its home network",
      NULL
    },
    { "3gpp-register-in-operator", 0, 0, G_OPTION_ARG_STRING, &register_in_operator_str,
      "Request a given modem to register in the network of the given operator",
      "[MCCMNC]"
    },
    { NULL }
};

GOptionGroup *
mmcli_modem_3gpp_get_option_group (void)
{
	GOptionGroup *group;

	group = g_option_group_new ("3gpp",
	                            "3GPP options",
	                            "Show 3GPP related options",
	                            NULL,
	                            NULL);
	g_option_group_add_entries (group, entries);

	return group;
}

gboolean
mmcli_modem_3gpp_options_enabled (void)
{
    static guint n_actions = 0;
    static gboolean checked = FALSE;

    if (checked)
        return !!n_actions;

    n_actions = (scan_flag +
                 register_home_flag +
                 !!register_in_operator_str);

    if (n_actions > 1) {
        g_printerr ("error: too many 3GPP actions requested\n");
        exit (EXIT_FAILURE);
    }

    /* Scanning networks takes really a long time, so we do it asynchronously
     * always to avoid DBus timeouts */
    if (scan_flag)
        mmcli_force_async_operation ();

    checked = TRUE;
    return !!n_actions;
}

static void
context_free (Context *ctx)
{
    if (!ctx)
        return;

    if (ctx->cancellable)
        g_object_unref (ctx->cancellable);
    if (ctx->modem_3gpp)
        g_object_unref (ctx->modem_3gpp);
    if (ctx->object)
        g_object_unref (ctx->object);
    if (ctx->manager)
        g_object_unref (ctx->manager);
    g_free (ctx);
}

void
mmcli_modem_3gpp_shutdown (void)
{
    context_free (ctx);
}

static void
print_network_info (MMModem3gppNetwork *network)
{
    const gchar *name;
    gchar *access_technologies;

    /* Not the best thing to do, as we may be doing _get() calls twice, but
     * easiest to maintain */
#undef VALIDATE
#define VALIDATE(str) (str ? str : "unknown")

    access_technologies = (mm_modem_get_access_technologies_string (
                               mm_modem_3gpp_network_get_access_technology (network)));

    /* Prefer long name */
    name = mm_modem_3gpp_network_get_operator_long (network);
    if (!name)
        name = mm_modem_3gpp_network_get_operator_short (network);

    g_print ("%s - %s (%s, %s)\n",
             VALIDATE (mm_modem_3gpp_network_get_operator_code (network)),
             VALIDATE (name),
             access_technologies,
             mmcli_get_3gpp_network_availability_string (
                 mm_modem_3gpp_network_get_availability (network)));
    g_free (access_technologies);
}

static void
scan_process_reply (GList *result,
                    const GError *error)
{
    if (!result) {
        g_printerr ("error: couldn't scan networks in the modem: '%s'\n",
                    error ? error->message : "unknown error");
        exit (EXIT_FAILURE);
    }

    g_print ("\n");
    if (!result)
        g_print ("No networks were found\n");
    else {
        GList *l;

        g_print ("Found %u networks:\n", g_list_length (result));
        for (l = result; l; l = g_list_next (l)) {
            print_network_info ((MMModem3gppNetwork *)(l->data));
        }
        g_list_free_full (result, (GDestroyNotify) mm_modem_3gpp_network_free);
    }
    g_print ("\n");
}

static void
scan_ready (MMModem3gpp  *modem_3gpp,
            GAsyncResult *result,
            gpointer      nothing)
{
    GList *operation_result;
    GError *error = NULL;

    operation_result = mm_modem_3gpp_scan_finish (modem_3gpp, result, &error);
    scan_process_reply (operation_result, error);

    mmcli_async_operation_done ();
}

static void
register_process_reply (gboolean result,
                        const GError *error)
{
    if (!result) {
        g_printerr ("error: couldn't register the modem: '%s'\n",
                    error ? error->message : "unknown error");
        exit (EXIT_FAILURE);
    }

    g_print ("successfully registered the modem\n");
}

static void
register_ready (MMModem3gpp  *modem_3gpp,
                GAsyncResult *result,
                gpointer      nothing)
{
    gboolean operation_result;
    GError *error = NULL;

    operation_result = mm_modem_3gpp_register_finish (modem_3gpp, result, &error);
    register_process_reply (operation_result, error);

    mmcli_async_operation_done ();
}

static void
get_modem_ready (GObject      *source,
                 GAsyncResult *result,
                 gpointer      none)
{
    ctx->object = mmcli_get_modem_finish (result, &ctx->manager);
    ctx->modem_3gpp = mm_object_get_modem_3gpp (ctx->object);

    /* Request to scan networks? */
    if (scan_flag) {
        g_debug ("Asynchronously scanning for networks...");
        mm_modem_3gpp_scan (ctx->modem_3gpp,
                            ctx->cancellable,
                            (GAsyncReadyCallback)scan_ready,
                            NULL);
        return;
    }

    /* Request to register the modem? */
    if (register_in_operator_str || register_home_flag) {
        g_debug ("Asynchronously registering the modem...");
        mm_modem_3gpp_register (ctx->modem_3gpp,
                                (register_in_operator_str ? register_in_operator_str : ""),
                                ctx->cancellable,
                                (GAsyncReadyCallback)register_ready,
                                NULL);
        return;
    }

    g_warn_if_reached ();
}

void
mmcli_modem_3gpp_run_asynchronous (GDBusConnection *connection,
                                   GCancellable    *cancellable)
{
    /* Initialize context */
    ctx = g_new0 (Context, 1);
    if (cancellable)
        ctx->cancellable = g_object_ref (cancellable);

    /* Get proper modem */
    mmcli_get_modem  (connection,
                      mmcli_get_common_modem_string (),
                      cancellable,
                      (GAsyncReadyCallback)get_modem_ready,
                      NULL);
}

void
mmcli_modem_3gpp_run_synchronous (GDBusConnection *connection)
{
    GError *error = NULL;

    /* Initialize context */
    ctx = g_new0 (Context, 1);
    ctx->object = mmcli_get_modem_sync (connection,
                                        mmcli_get_common_modem_string (),
                                        &ctx->manager);
    ctx->modem_3gpp = mm_object_get_modem_3gpp (ctx->object);

    if (scan_flag)
        g_assert_not_reached ();

    /* Request to register the modem? */
    if (register_in_operator_str || register_home_flag) {
        gboolean result;

        g_debug ("Synchronously registering the modem...");
        result = mm_modem_3gpp_register_sync (
            ctx->modem_3gpp,
            (register_in_operator_str ? register_in_operator_str : ""),
            NULL,
            &error);
        register_process_reply (result, error);
        return;
    }

    g_warn_if_reached ();
}
