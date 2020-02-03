/*
 * /net/reactivated/Fprint/Manager object implementation
 * Copyright (C) 2008 Daniel Drake <dsd@gentoo.org>
 * Copyright (C) 2020 Marco Trevisan <marco.trevisan@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <unistd.h>
#include <stdlib.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <fprint.h>
#include <glib-object.h>

#include "fprintd.h"

static void fprint_manager_constructed (GObject *object);
static gboolean fprint_manager_get_devices(FprintManager *manager,
	GPtrArray **devices, GError **error);
static gboolean fprint_manager_get_default_device(FprintManager *manager,
	const char **device, GError **error);

typedef struct
{
	GDBusConnection *connection;
	FprintDBusManager *dbus_manager;
	FpContext *context;
	GSList *dev_registry;
	gboolean no_timeout;
	guint timeout_id;
} FprintManagerPrivate;

G_DEFINE_TYPE_WITH_CODE(FprintManager, fprint_manager, G_TYPE_OBJECT, G_ADD_PRIVATE (FprintManager))

enum {
	PROP_0,
	FPRINT_MANAGER_CONNECTION,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void fprint_manager_finalize(GObject *object)
{
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (FPRINT_MANAGER (object));

	g_clear_object (&priv->dbus_manager);
	g_clear_object (&priv->connection);
	g_clear_object (&priv->context);
	g_slist_free(priv->dev_registry);

	G_OBJECT_CLASS(fprint_manager_parent_class)->finalize(object);
}

static void fprint_manager_set_property (GObject *object, guint property_id,
					 const GValue *value, GParamSpec *pspec)
{
	FprintManager *self = FPRINT_MANAGER (object);
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (self);

	switch (property_id) {
	case FPRINT_MANAGER_CONNECTION:
		priv->connection = g_value_dup_object (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void fprint_manager_get_property (GObject *object, guint property_id,
					 GValue *value, GParamSpec *pspec)
{
	FprintManager *self = FPRINT_MANAGER (object);
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (self);

	switch (property_id) {
	case FPRINT_MANAGER_CONNECTION:
		g_value_set_object (value, priv->connection);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void fprint_manager_class_init(FprintManagerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->constructed = fprint_manager_constructed;
	object_class->set_property = fprint_manager_set_property;
	object_class->get_property = fprint_manager_get_property;
	object_class->finalize = fprint_manager_finalize;

	properties[FPRINT_MANAGER_CONNECTION] =
		g_param_spec_object ("connection",
				     "Connection",
				     "Set GDBus connection property",
				     G_TYPE_DBUS_CONNECTION,
				     G_PARAM_CONSTRUCT_ONLY |
				     G_PARAM_READWRITE);

	g_object_class_install_properties (object_class, N_PROPS, properties);
}

static gchar *get_device_path(FprintDevice *rdev)
{
	return g_strdup_printf (FPRINT_SERVICE_PATH "/Device/%d",
		_fprint_device_get_id(rdev));
}

static gboolean
fprint_manager_timeout_cb (FprintManager *manager)
{
	//FIXME kill all the devices
	exit(0);
	return FALSE;
}

static void
fprint_manager_in_use_notified (FprintDevice *rdev, GParamSpec *spec, FprintManager *manager)
{
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (manager);
	guint num_devices_used = 0;
	GSList *l;
	gboolean in_use;

	if (priv->timeout_id > 0) {
		g_source_remove (priv->timeout_id);
		priv->timeout_id = 0;
	}
	if (priv->no_timeout)
		return;

	for (l = priv->dev_registry; l != NULL; l = l->next) {
		FprintDevice *dev = l->data;

		g_object_get (G_OBJECT(dev), "in-use", &in_use, NULL);
		if (in_use != FALSE)
			num_devices_used++;
	}

	if (num_devices_used == 0)
		priv->timeout_id = g_timeout_add_seconds (TIMEOUT, (GSourceFunc) fprint_manager_timeout_cb, manager);
}

static gboolean
handle_get_devices (FprintManager *manager, GDBusMethodInvocation *invocation,
		    FprintDBusManager *skeleton)
{
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GError) error = NULL;

	if (!fprint_manager_get_devices (manager, &devices, &error)) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		return TRUE;
	}

	fprint_dbus_manager_complete_get_devices (skeleton, invocation,
						  (const gchar *const *)
						  devices->pdata);

	return TRUE;
}

static gboolean
handle_get_default_device (FprintManager *manager,
			   GDBusMethodInvocation *invocation,
			   FprintDBusManager *skeleton)
{
	const gchar *device;
	g_autoptr(GError) error = NULL;

	if (!fprint_manager_get_default_device (manager, &device, &error)) {
		g_dbus_method_invocation_return_gerror (invocation, error);
		return TRUE;
	}

	fprint_dbus_manager_complete_get_default_device (skeleton, invocation,
							 device);

	return TRUE;
}

static void
device_added_cb (FprintManager *manager, FpDevice *device, FpContext *context)
{
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (manager);
	FprintDevice *rdev = fprint_device_new(device);
	g_autofree gchar *path = NULL;

	g_signal_connect (G_OBJECT(rdev), "notify::in-use",
			  G_CALLBACK (fprint_manager_in_use_notified), manager);

	priv->dev_registry = g_slist_prepend (priv->dev_registry, rdev);
	path = get_device_path (rdev);
	g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (rdev),
					  priv->connection,
					  path, NULL);
}

static void
device_removed_cb (FprintManager *manager, FpDevice *device, FpContext *context)
{
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (manager);
	GSList *item;

	for (item = priv->dev_registry; item; item = item->next) {
		FprintDevice *rdev;
		g_autoptr(FpDevice) dev = NULL;

		rdev = item->data;

		g_object_get (rdev, "dev", &dev, NULL);
		if (dev != device)
			continue;

		priv->dev_registry = g_slist_delete_link (priv->dev_registry, item);

		g_dbus_interface_skeleton_unexport (
			G_DBUS_INTERFACE_SKELETON (rdev));

		g_signal_handlers_disconnect_by_data (rdev, manager);
		g_object_unref (rdev);

		/* We cannot continue to iterate at this point, but we don't need to either */
		break;
	}

	/* The device that disappeared might have been in-use.
	 * Do we need to do anything else in this case to clean up more gracefully? */
	fprint_manager_in_use_notified (NULL, NULL, manager);
}

static void fprint_manager_constructed (GObject *object)
{
	FprintManager *manager = FPRINT_MANAGER (object);
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (manager);

	priv->dbus_manager = fprint_dbus_manager_skeleton_new ();
	priv->context = fp_context_new ();

	g_signal_connect_object (priv->dbus_manager,
				 "handle-get-devices",
				 G_CALLBACK (handle_get_devices),
				 manager,
				 G_CONNECT_SWAPPED);
	g_signal_connect_object (priv->dbus_manager,
				 "handle-get-default-device",
				 G_CALLBACK (handle_get_default_device),
				 manager,
				 G_CONNECT_SWAPPED);

	g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (priv->dbus_manager),
					  priv->connection,
					  FPRINT_SERVICE_PATH "/Manager", NULL);

	/* And register the signals for initial enumeration and hotplug. */
	g_signal_connect_object (priv->context,
				 "device-added",
				 (GCallback) device_added_cb,
				 manager,
				 G_CONNECT_SWAPPED);

	g_signal_connect_object (priv->context,
				 "device-removed",
				 (GCallback) device_removed_cb,
				 manager,
				 G_CONNECT_SWAPPED);

	/* Prepare everything by enumerating all devices.
	 * This blocks the main loop until the existing devices are enumerated
	 */
	fp_context_enumerate (priv->context);

	G_OBJECT_CLASS (fprint_manager_parent_class)->constructed (object);
}

static void
fprint_manager_init (FprintManager *manager)
{
}

FprintManager *fprint_manager_new (GDBusConnection *connection, gboolean no_timeout)
{
	FprintManagerPrivate *priv;
	GObject *object;

	object = g_object_new (FPRINT_TYPE_MANAGER, "connection", connection, NULL);
	priv = fprint_manager_get_instance_private (FPRINT_MANAGER (object));
	priv->no_timeout = no_timeout;

	if (!priv->no_timeout)
		priv->timeout_id = g_timeout_add_seconds (TIMEOUT, (GSourceFunc) fprint_manager_timeout_cb, object);

	return FPRINT_MANAGER (object);
}

static gboolean fprint_manager_get_devices(FprintManager *manager,
	GPtrArray **devices, GError **error)
{
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (manager);
	GSList *elem;
	GSList *l;
	int num_open;
	GPtrArray *devs;

	elem = g_slist_reverse(g_slist_copy(priv->dev_registry));
	num_open = g_slist_length(elem);
	devs = g_ptr_array_sized_new(num_open);

	if (num_open > 0) {
		for (l = elem; l != NULL; l = l->next) {
			GDBusInterfaceSkeleton *dev_skeleton = l->data;
			const char *path;

			path = g_dbus_interface_skeleton_get_object_path (
				dev_skeleton);
			g_ptr_array_add (devs, (char *) path);
		}
	}
	g_ptr_array_add (devs, NULL);

	g_slist_free(elem);

	*devices = devs;
	return TRUE;
}

static gboolean fprint_manager_get_default_device(FprintManager *manager,
	const char **device, GError **error)
{
	FprintManagerPrivate *priv = fprint_manager_get_instance_private (manager);
	GSList *elem;;
	int num_open;

	elem = priv->dev_registry;
	num_open = g_slist_length(elem);

	if (num_open > 0) {
		GDBusInterfaceSkeleton *dev_skeleton = g_slist_last (elem)->data;
		*device = g_dbus_interface_skeleton_get_object_path (dev_skeleton);
		return TRUE;
	} else {
		g_set_error (error, FPRINT_ERROR, FPRINT_ERROR_NO_SUCH_DEVICE,
			     "No devices available");
		*device = NULL;
		return FALSE;
	}
}

#define ERROR_ENTRY(name, dbus_name) \
	{ FPRINT_ERROR_ ## name, FPRINT_ERROR_DBUS_INTERFACE "." dbus_name }
GDBusErrorEntry fprint_error_entries[] =
{
	ERROR_ENTRY (CLAIM_DEVICE, "ClaimDevice"),
	ERROR_ENTRY (ALREADY_IN_USE, "AlreadyInUse"),
	ERROR_ENTRY (INTERNAL, "Internal"),
	ERROR_ENTRY (PERMISSION_DENIED, "PermissionDenied"),
	ERROR_ENTRY (NO_ENROLLED_PRINTS, "NoEnrolledPrints"),
	ERROR_ENTRY (NO_ACTION_IN_PROGRESS, "NoActionInProgress"),
	ERROR_ENTRY (INVALID_FINGERNAME, "InvalidFingername"),
	ERROR_ENTRY (NO_SUCH_DEVICE, "NoSuchDevice"),
};

GQuark fprint_error_quark (void)
{
	static volatile gsize quark = 0;
	if (!quark) {
		g_dbus_error_register_error_domain ("fprintd-error-quark",
						    &quark,
						    fprint_error_entries,
						    G_N_ELEMENTS (fprint_error_entries));
	}
	return (GQuark) quark;
}
