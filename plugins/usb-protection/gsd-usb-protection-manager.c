/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2018 Ludovico de Nittis <denittis@gnome.org>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <gio/gio.h>
#include <glib-object.h>
#include <glib/gi18n.h>
#include <libnotify/notify.h>
#include <locale.h>
#include <string.h>

#include <gdesktop-enums.h>

#include "gnome-settings-bus.h"
#include "gnome-settings-profile.h"
#include "gsd-enums.h"
#include "gsd-usb-protection-manager.h"

#define PRIVACY_SETTINGS "org.gnome.desktop.privacy"
#define USB_PROTECTION "usb-protection"
#define USB_PROTECTION_LEVEL "usb-protection-level"

#define DBUS_VERSION "1"

#define USBGUARD_DBUS_NAME "org.usbguard" DBUS_VERSION
#define USBGUARD_DBUS_PATH "/org/usbguard" DBUS_VERSION
#define USBGUARD_DBUS_INTERFACE "org.usbguard"
#define USBGUARD_DBUS_INTERFACE_VERSIONED USBGUARD_DBUS_INTERFACE DBUS_VERSION

#define USBGUARD_DBUS_PATH_POLICY USBGUARD_DBUS_PATH "/Policy"
#define USBGUARD_DBUS_INTERFACE_POLICY USBGUARD_DBUS_INTERFACE ".Policy" DBUS_VERSION

#define USBGUARD_DBUS_PATH_DEVICES USBGUARD_DBUS_PATH "/Devices"
#define USBGUARD_DBUS_INTERFACE_DEVICES USBGUARD_DBUS_INTERFACE ".Devices" DBUS_VERSION

#define APPLY_POLICY "apply-policy"
#define BLOCK "block"
#define REJECT "reject"

#define APPLY_DEVICE_POLICY "applyDevicePolicy"
#define LIST_DEVICES "listDevices"
#define LIST_RULES "listRules"
#define ALLOW "allow"
#define DEVICE_POLICY_CHANGED "DevicePolicyChanged"
#define DEVICE_PRESENCE_CHANGED "DevicePresenceChanged"
#define INSERTED_DEVICE_POLICY "InsertedDevicePolicy"
#define APPEND_RULE "appendRule"
#define ALLOW_ALL "allow id *:* label \"GNOME_SETTINGS_DAEMON_RULE\""
#define WITH_CONNECT_TYPE "with-connect-type"
#define WITH_INTERFACE "with-interface"
#define NAME "name"

struct _GsdUsbProtectionManager
{
        GObject             parent;
        guint               start_idle_id;
        GDBusNodeInfo      *introspection_data;
        GSettings          *settings;
        guint               name_id;
        GDBusConnection    *connection;
        gboolean            available;
        GDBusProxy         *usb_protection;
        GDBusProxy         *usb_protection_devices;
        GDBusProxy         *usb_protection_policy;
        GCancellable       *cancellable;
        GsdScreenSaver     *screensaver_proxy;
        gboolean            screensaver_active;
        NotifyNotification *notification;
};

typedef enum {
        EVENT_PRESENT,
        EVENT_INSERT,
        EVENT_UPDATE,
        EVENT_REMOVE
} UsbGuardEvent;


typedef enum {
        TARGET_ALLOW,
        TARGET_BLOCK,
        TARGET_REJECT
} UsbGuardTarget;

typedef enum {
        POLICY_DEVICE_ID,
        POLICY_TARGET_OLD,
        /* This is the rule that has been applied */
        POLICY_TARGET_NEW,
        POLICY_DEV_RULE,
        /* The ID of the rule that has been applied.
         * uint32 - 1 is one of the implicit rules,
         * e.g. ImplicitPolicyTarget or InsertedDevicePolicy.
         */
        POLICY_RULE_ID,
        POLICY_ATTRIBUTES
} UsbGuardPolicyChanged;

typedef enum {
        PRESENCE_DEVICE_ID,
        PRESENCE_EVENT,
        /* That does not reflect what USBGuard intends to do with the device :( */
        PRESENCE_TARGET,
        PRESENCE_DEV_RULE,
        PRESENCE_ATTRIBUTES
} UsbGuardPresenceChanged;

static void gsd_usb_protection_manager_finalize (GObject *object);

G_DEFINE_TYPE (GsdUsbProtectionManager, gsd_usb_protection_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;

#define GSD_DBUS_NAME "org.gnome.SettingsDaemon"
#define GSD_DBUS_PATH "/org/gnome/SettingsDaemon"
#define GSD_DBUS_BASE_INTERFACE "org.gnome.SettingsDaemon"

#define GSD_USB_PROTECTION_DBUS_NAME GSD_DBUS_NAME ".UsbProtection"
#define GSD_USB_PROTECTION_DBUS_PATH GSD_DBUS_PATH "/UsbProtection"

static const gchar introspection_xml[] =
"<node>"
"  <interface name='org.gnome.SettingsDaemon.UsbProtection'>"
"    <property name='Available' type='b' access='read'/>"
"  </interface>"
"</node>";

static void
dbus_call_log_error (GObject      *source_object,
                     GAsyncResult *res,
                     gpointer      user_data)
{
        g_autoptr(GVariant) result = NULL;
        g_autoptr(GError) error = NULL;
        const gchar *msg = user_data;

        result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object),
                                           res,
                                           &error);
        if (result == NULL)
                g_warning ("%s: %s", msg, error->message);
}

static void
add_usbguard_allow_rule (GsdUsbProtectionManager *manager)
{
        /* This appends a "allow all" rule.
         * It has the purpose of ensuring the authorization of new devices when
         * the lockscreen is off while respecting existing rules.
         * We make it temporary, so that we are stateless and don't alter the
         * existing (persistent) configuration.
         */

        GVariant *params;
        GDBusProxy *policy_proxy = manager->usb_protection_policy;

        if (policy_proxy == NULL) {
            g_warning ("Cannot add allow rule, because dbus proxy is missing");
        } else {
                gboolean temporary = TRUE;
                /* This is USBGuard's Rule::LastID */
                /* const guint32 last_rule_id = G_MAXUINT32 - 2; */
                /* We can't use Rule::LastID, due to a bug in USBGuard.
                 * We cannot pick an arbitrary number, so we pick
                 * "0" which means we prepend our rule.
                 * https://github.com/USBGuard/usbguard/pull/355
                 */
                const guint32 last_rule_id = 0;
                g_debug ("Adding rule %u", last_rule_id);
                params = g_variant_new ("(sub)", ALLOW_ALL, last_rule_id, temporary);
                g_dbus_proxy_call (policy_proxy,
                                   APPEND_RULE,
                                   params,
                                   G_DBUS_CALL_FLAGS_NONE,
                                   -1,
                                   manager->cancellable,
                                   dbus_call_log_error,
                                   "Error appending USBGuard rule");
        }
}

static gboolean
is_usbguard_allow_rule_present (GVariant *rules)
{
        g_autoptr(GVariantIter) iter = NULL;
        g_autofree gchar *value = NULL;
        guint number = 0;

        g_debug ("Detecting rule...");

        g_variant_get (rules, "a(us)", &iter);
        while (g_variant_iter_loop (iter, "(us)", &number, &value)) {
                if (g_strcmp0 (value, ALLOW_ALL) == 0) {
                        g_debug ("Detected rule!");
                        return TRUE;
                    }
        }
        return FALSE;
}

static void
usbguard_listrules_cb (GObject      *source_object,
                       GAsyncResult *res,
                       gpointer      user_data)
{
        GVariant *result, *rules;
        g_autoptr(GError) error = NULL;

        result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object),
                                           res,
                                           &error);

        if (!result) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                        g_warning ("Failed to fetch USBGuard rules list: %s", error->message);
                }
                return;
        }

        rules = g_variant_get_child_value (result, 0);
        g_variant_unref (result);
        if (!is_usbguard_allow_rule_present (rules))
                add_usbguard_allow_rule (user_data);

}

static void
usbguard_ensure_allow_rule (GsdUsbProtectionManager *manager)
{
        GVariant *params;
        GDBusProxy *policy_proxy = manager->usb_protection_policy;

        if (policy_proxy == NULL) {
            g_warning ("Cannot list rules, because dbus proxy is missing");
        } else {
                /* listRules parameter is a label for matching rules.
                 * Currently we are using an empty label to get all the
                 * rules instead of just using "GNOME_SETTINGS_DAEMON_RULE"
                 * until this bug gets solved:
                 * https://github.com/USBGuard/usbguard/issues/328 */
                params = g_variant_new ("(s)", "");
                g_dbus_proxy_call (policy_proxy,
                                   LIST_RULES,
                                   params,
                                   G_DBUS_CALL_FLAGS_NONE,
                                   -1,
                                   manager->cancellable,
                                   usbguard_listrules_cb,
                                   manager);
        }
}

static void
settings_changed_callback (GSettings               *settings,
                           const char              *key,
                           GsdUsbProtectionManager *manager)
{
        gchar *value_usbguard;
        gboolean usbguard_controlled;
        GVariant *params;
        GDesktopUsbProtection protection_level;

        /* We react only if one of the two USB related properties has been changed */
        if (g_strcmp0 (key, USB_PROTECTION) != 0 && g_strcmp0 (key, USB_PROTECTION_LEVEL) != 0)
                return;

        usbguard_controlled = g_settings_get_boolean (settings, USB_PROTECTION);
        protection_level = g_settings_get_enum (settings, USB_PROTECTION_LEVEL);
        g_debug ("USBGuard control is currently %i with a protection level of %i",
                 usbguard_controlled, protection_level);

        /* If previously we were controlling USBGuard and now we are not,
         * we leave the USBGuard configuration in a clean state. I.e. we set
         * "InsertedDevicePolicy" to "apply-policy" and we ensure that
         * there is an always allow rule. In this way even if USBGuard daemon
         * is running every USB devices will be automatically authorized. */
        if (g_strcmp0 (key, USB_PROTECTION) == 0 && !usbguard_controlled) {
                g_debug ("let's clean usbguard config state");
                params = g_variant_new ("(ss)",
                                        INSERTED_DEVICE_POLICY,
                                        APPLY_POLICY);

                if (manager->usb_protection != NULL) {
                        g_dbus_proxy_call (manager->usb_protection,
                                           "setParameter",
                                           params,
                                           G_DBUS_CALL_FLAGS_NONE,
                                           -1,
                                           manager->cancellable,
                                           dbus_call_log_error,
                                           "Error calling USBGuard DBus to set a clean configuration state");
                }

                usbguard_ensure_allow_rule (manager);
        }

        /* Only if we are entitled to handle USBGuard */
        if (usbguard_controlled && manager->usb_protection != NULL) {
                value_usbguard = (protection_level == G_DESKTOP_USB_PROTECTION_ALWAYS) ? BLOCK : APPLY_POLICY;
                params = g_variant_new ("(ss)",
                                        INSERTED_DEVICE_POLICY,
                                        value_usbguard);

                g_dbus_proxy_call (manager->usb_protection,
                                   "setParameter",
                                   params,
                                   G_DBUS_CALL_FLAGS_NONE,
                                   -1,
                                   manager->cancellable,
                                   dbus_call_log_error,
                                   "Error calling USBGuard DBus to set the desidered protection level");

                /* If we are in "When lockscreen is active" we also check if the
                 * always allow rule is present. */
                if (protection_level == G_DESKTOP_USB_PROTECTION_LOCKSCREEN)
                        usbguard_ensure_allow_rule (manager);
        }
}

static void update_usb_protection_store (GsdUsbProtectionManager *manager,
                                         GVariant                *parameter)
{
        const gchar *key;
        gboolean usbguard_controlled;
        GDesktopUsbProtection protection_level;
        GSettings *settings = manager->settings;

        usbguard_controlled = g_settings_get_boolean (settings, USB_PROTECTION);
        /* If we are not handling USBGuard configuration (e.g. the user is using
         * a third party program) we do nothing when the config changes. */
        if (usbguard_controlled) {
                key = g_variant_get_string (parameter, NULL);
                protection_level = g_settings_get_enum (settings, USB_PROTECTION_LEVEL);
                /* If the USBGuard configuration has been changed and doesn't match
                 * our internal state, most likely means that the user externally
                 * changed it. When this happens we set to false the control value. */
                if ((g_strcmp0 (key, APPLY_POLICY) == 0 && protection_level == G_DESKTOP_USB_PROTECTION_ALWAYS)) {
                        g_settings_set (settings, USB_PROTECTION, "b", FALSE);
                        g_warning ("We don't control anymore USBGuard because the configuration changed externally.");
                }
        }
}

static gboolean
is_protection_active (GsdUsbProtectionManager *manager)
{
        GSettings *settings = manager->settings;

        return g_settings_get_boolean (settings, USB_PROTECTION);
}

static void
on_notification_closed (NotifyNotification      *n,
                        GsdUsbProtectionManager *manager)
{
        g_clear_object (&manager->notification);
}

static void
show_notification (GsdUsbProtectionManager *manager,
                   const char              *summary,
                   const char              *body)
{
        /* Don't show a notice if one is already displayed */
        if (manager->notification != NULL)
                return;

        manager->notification = notify_notification_new (summary, body, "drive-removable-media-symbolic");
        notify_notification_set_app_name (manager->notification, _("USB Protection"));
        notify_notification_set_hint (manager->notification, "transient", g_variant_new_boolean (TRUE));
        notify_notification_set_hint_string (manager->notification, "x-gnome-privacy-scope", "system");
        notify_notification_set_timeout (manager->notification, NOTIFY_EXPIRES_DEFAULT);
        notify_notification_set_urgency (manager->notification, NOTIFY_URGENCY_CRITICAL);
        g_signal_connect_object (manager->notification,
                                 "closed",
                                 G_CALLBACK (on_notification_closed),
                                 manager,
                                 0);
        if (!notify_notification_show (manager->notification, NULL)) {
                g_warning ("Failed to send USB protection notification");
                g_clear_object (&manager->notification);
        }
}

static void authorize_device (GDBusProxy              *proxy,
                              GsdUsbProtectionManager *manager,
                              guint                    device_id,
                              guint                    target,
                              gboolean                 permanent)
{
        if (manager->usb_protection_devices == NULL) {
            g_warning("Could not authorize device, because DBus is missing");
        } else {
            GVariant *params = g_variant_new ("(uub)", device_id, target, permanent);
            g_dbus_proxy_call (manager->usb_protection_devices,
                               APPLY_DEVICE_POLICY,
                               params,
                               G_DBUS_CALL_FLAGS_NONE,
                               -1,
                               manager->cancellable,
                               dbus_call_log_error,
                               "Error calling USBGuard DBus to authorize a device");
        }
}

static gboolean
is_only_hid (GVariant *device)
{
        g_autoptr(GVariantIter) iter = NULL;
        g_autofree gchar *name = NULL;
        g_autofree gchar *value = NULL;
        guint i;
        gboolean only_hid = TRUE;

        g_variant_get_child (device, POLICY_ATTRIBUTES, "a{ss}", &iter);
        while (g_variant_iter_loop (iter, "{ss}", &name, &value)) {
                if (g_strcmp0 (name, WITH_INTERFACE) == 0) {
                        g_auto(GStrv) interfaces_splitted = NULL;
                        interfaces_splitted = g_strsplit (value, " ", -1);
                        for (i = 0; i < g_strv_length (interfaces_splitted); i++)
                                if (!g_str_has_prefix (interfaces_splitted[i], "03:"))
                                        only_hid = FALSE;
                }
        }
        return only_hid;
}

static gboolean
is_hardwired (GVariant *device)
{
        g_autoptr(GVariantIter) iter = NULL;
        g_autofree gchar *name = NULL;
        g_autofree gchar *value = NULL;

        g_variant_get_child (device, PRESENCE_ATTRIBUTES, "a{ss}", &iter);
        while (g_variant_iter_loop (iter, "{ss}", &name, &value)) {
                if (g_strcmp0 (name, WITH_CONNECT_TYPE) == 0) {
                        return g_strcmp0 (value, "hardwired") == 0;
                }
        }
        return FALSE;
}

static gboolean
is_keyboard (GVariant *device)
{
        g_autoptr(GVariantIter) iter = NULL;
        g_autofree gchar *name = NULL;
        g_autofree gchar *value = NULL;

        g_variant_get_child (device, PRESENCE_ATTRIBUTES, "a{ss}", &iter);
        while (g_variant_iter_loop (iter, "{ss}", &name, &value)) {
                if (g_strcmp0 (name, WITH_INTERFACE) == 0) {
                        return g_strrstr (value, "03:00:01") != NULL ||
                               g_strrstr (value, "03:01:01") != NULL;
                }
        }
        return FALSE;
}

static void
auth_device (GsdUsbProtectionManager *manager,
               GVariant                *device)
{
        guint device_id;

        if (manager->usb_protection_devices == NULL)
                return;

        g_variant_get_child (device, POLICY_DEVICE_ID, "u", &device_id);
        authorize_device(manager->usb_protection_devices,
                         manager,
                         device_id,
                         TARGET_ALLOW,
                         FALSE);
}

static void
on_screen_locked (GsdScreenSaver          *screen_saver,
                  GAsyncResult            *result,
                  GsdUsbProtectionManager *manager)
{
        gboolean is_locked;
        g_autoptr(GError) error = NULL;

        is_locked = gsd_screen_saver_call_lock_finish (screen_saver, result, &error);

        if (error) {
            g_warning ("Couldn't lock screen: %s", error->message);
            if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                return;
            }
        }

        show_notification (manager,
                           _("New USB device"),
                           _("New device has been detected while the session was not locked. "
                             "If you did not plug anything, check your system for any suspicious device."));

}

static void
on_usbguard_signal (GDBusProxy *proxy,
                           gchar      *sender_name,
                           gchar      *signal_name,
                           GVariant   *parameters,
                           gpointer    user_data)
{
        UsbGuardTarget target = TARGET_BLOCK;
        UsbGuardEvent device_event;
        GDesktopUsbProtection protection_level;
        GsdUsbProtectionManager *manager = user_data;
        g_autoptr(GVariantIter) iter = NULL;
        g_autofree gchar *name = NULL;
        g_autofree gchar *device_name = NULL;

        g_debug ("USBGuard signal: %s", signal_name);

        /* We act only if we receive a signal indicating that a device has been inserted */
        if (g_strcmp0 (signal_name, DEVICE_PRESENCE_CHANGED) != 0) {
                return;
        }

        g_variant_get_child (parameters, PRESENCE_EVENT, "u", &device_event);
        if (device_event != EVENT_INSERT) {
            g_debug ("Device hat not been inserted (%d); ignoring", device_event);
            return;
        }

        /* We would like to show a notification for an inserted device that
         * *has not been blocked*.  But USBGuard is not providing that information.
         * So we have to work around that limitation and assume that any device plugged in
         * during screensaver shall be blocked.
         * https://github.com/USBGuard/usbguard/issues/353
         
           g_variant_get_child (parameters, POLICY_TARGET_NEW, "u", &target);
        */

        /* If the device is already authorized we do nothing */
        if (target == TARGET_ALLOW) {
                g_debug ("Device will be allowed, we return");
                return;
        }

        /* If the USB protection is disabled we do nothing */
        if (!is_protection_active (manager)) {
                g_debug ("Protection is not active. Not acting on the device");
                return;
        }

        g_variant_get_child (parameters, PRESENCE_ATTRIBUTES, "a{ss}", &iter);
        while (g_variant_iter_loop (iter, "{ss}", &name, &device_name)) {
                if (g_strcmp0 (name, NAME) == 0)
                        g_debug ("A new USB device has been connected: %s", device_name);
        }

        if (is_hardwired (parameters)) {
            g_debug ("Device is hardwired, allowing it to be connected");
            auth_device (manager, parameters);
            return;
        }

        protection_level = g_settings_get_enum (manager->settings, USB_PROTECTION_LEVEL);

        g_debug ("Screensaver active: %d", manager->screensaver_active);
        if (manager->screensaver_active) {
                /* If the session is locked we check if the inserted device is a keyboard.
                 * If that is the case we authorize the newly inserted keyboard as an
                 * antilockout policy.
                 *
                 * If this keyboard advertises also interfaces outside the HID class it is suspect.
                 * It could be a false positive because this could be a "smart" keyboard, but at
                 * this stage is better be safe. */
                if (is_keyboard (parameters) && is_only_hid (parameters)) {
                        show_notification (manager,
                                           _("New keyboard detected"),
                                           _("Either your keyboard has been reconnected or a new one has been plugged in. "
                                             "If you did not do it, check your system for any suspicious device."));
                        auth_device (manager, parameters);
                } else {
                    if (protection_level == G_DESKTOP_USB_PROTECTION_LOCKSCREEN) {
                            show_notification (manager,
                                               _("Reconnect USB device"),
                                               _("New device has been detected while you were away. "
                                                 "Please disconnect and reconnect the device to start using it."));
                    } else {
                            const char* name_for_notification = device_name ? device_name : "unknown name";
                            g_debug ("Showing notification for %s", name_for_notification);
                            show_notification (manager,
                                               _("USB device blocked"),
                                               _("New device has been detected while you were away. "
                                                 "It has been blocked because the USB protection is active."));
                    }
                }
        } else {
                /* If the protection level is "lockscreen" the device will be automatically
                 * authorized by usbguard. */
                if (protection_level == G_DESKTOP_USB_PROTECTION_ALWAYS) {
                        /* We authorize the device if this is a keyboard.
                         * We also lock the screen to prevent an attacker to plug malicious
                         * devices if the legitimate user forgot to lock his session.
                         *
                         * If this keyboard advertises also interfaces outside the HID class it is suspect.
                         * It could be a false positive because this could be a "smart" keyboard, but at
                         * this stage is better be safe. */
                        if (is_keyboard (parameters) && is_only_hid (parameters)) {
                                gsd_screen_saver_call_lock (manager->screensaver_proxy,
                                                            manager->cancellable,
                                                            (GAsyncReadyCallback) on_screen_locked,
                                                            manager);
                                auth_device (manager, parameters);
                        } else {
                                show_notification (manager,
                                                   _("USB device blocked"),
                                                   _("The new inserted device has been blocked because the USB protection is active."));
                        }
                }
            }
}

static void
on_usb_protection_signal (GDBusProxy *proxy,
                          gchar      *sender_name,
                          gchar      *signal_name,
                          GVariant   *parameters,
                          gpointer    user_data)
{
        g_autoptr(GVariant) parameter = NULL;
        g_autofree gchar *policy_name = NULL;

        if (g_strcmp0 (signal_name, "PropertyParameterChanged") != 0)
                return;

        g_variant_get_child (parameters, 0, "s", &policy_name);

        /* Right now we just care about the InsertedDevicePolicy value */
        if (g_strcmp0 (policy_name, INSERTED_DEVICE_POLICY) != 0)
                return;

        parameter = g_variant_get_child_value (parameters, 2);
        update_usb_protection_store (user_data, parameter);

}

static void
get_parameter_cb (GObject      *source_object,
                  GAsyncResult *res,
                  gpointer      user_data)
{
        GVariant *result;
        GVariant *params = NULL;
        g_autofree gchar *key = NULL;
        GDesktopUsbProtection protection_level;
        GsdUsbProtectionManager *manager;
        GSettings *settings;
        g_autoptr(GError) error = NULL;

        result = g_dbus_proxy_call_finish (G_DBUS_PROXY (source_object),
                                           res,
                                           &error);
        if (result == NULL) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                        g_warning ("Failed to fetch USBGuard parameters: %s", error->message);
                }
                return;
        }

        manager = GSD_USB_PROTECTION_MANAGER (user_data);
        settings = manager->settings;

        g_variant_get_child (result, 0, "s", &key);
        g_variant_unref (result);
        protection_level = g_settings_get_enum (settings, USB_PROTECTION_LEVEL);

        g_debug ("InsertedDevicePolicy is: %s", key);

        if (protection_level == G_DESKTOP_USB_PROTECTION_LOCKSCREEN) {
                if (g_strcmp0 (key, APPLY_POLICY) != 0) {
                        /* We are out of sync. */
                        params = g_variant_new ("(ss)",
                                                INSERTED_DEVICE_POLICY,
                                                APPLY_POLICY);
                }
        } else if (protection_level == G_DESKTOP_USB_PROTECTION_ALWAYS) {
                if (g_strcmp0 (key, BLOCK) != 0) {
                        /* We are out of sync. */
                        params = g_variant_new ("(ss)",
                                                INSERTED_DEVICE_POLICY,
                                                BLOCK);
                }
        }

        if (params != NULL) {
                /* We are out of sync. We need to call setParameter to update USBGuard state */
                if (manager->usb_protection != NULL) {
                        g_debug ("Setting InsertedDevicePolicy");
                        g_dbus_proxy_call (manager->usb_protection,
                                           "setParameter",
                                           params,
                                           G_DBUS_CALL_FLAGS_NONE,
                                           -1,
                                           manager->cancellable,
                                           dbus_call_log_error,
                                           "Error calling USBGuard DBus while we were out of sync");
                }

        }

        /* If we are in "When lockscreen is active" we also check
         * if the "always allow" rule is present. */
        if (protection_level == G_DESKTOP_USB_PROTECTION_LOCKSCREEN) {
                g_debug ("Ensuring allow all");
                usbguard_ensure_allow_rule (manager);
        }
}

static void
sync_usb_protection (GDBusProxy              *proxy,
                     GsdUsbProtectionManager *manager)
{
        GVariant *params;
        gboolean usbguard_controlled;
        GSettings *settings = manager->settings;

        usbguard_controlled = g_settings_get_boolean (settings, USB_PROTECTION);

        g_debug ("Attempting to sync USB parameters: %d %p %p",
            usbguard_controlled, proxy, manager->usb_protection);

        if (!usbguard_controlled || manager->usb_protection == NULL)
                return;

        params = g_variant_new ("(s)", INSERTED_DEVICE_POLICY);
        g_dbus_proxy_call (manager->usb_protection,
                           "getParameter",
                           params,
                           G_DBUS_CALL_FLAGS_NONE,
                           -1,
                           manager->cancellable,
                           get_parameter_cb,
                           manager);
}

static void
usb_protection_properties_changed (GsdUsbProtectionManager *manager)
{
        GVariantBuilder props_builder;
        GVariant *props_changed = NULL;

        /* not yet connected to the session bus */
        if (manager->connection == NULL)
                return;

        g_variant_builder_init (&props_builder, G_VARIANT_TYPE ("a{sv}"));

        g_variant_builder_add (&props_builder, "{sv}", "Available",
                               g_variant_new_boolean (manager->available));

        props_changed = g_variant_new ("(s@a{sv}@as)", GSD_USB_PROTECTION_DBUS_NAME,
                                       g_variant_builder_end (&props_builder),
                                       g_variant_new_strv (NULL, 0));

        g_dbus_connection_emit_signal (manager->connection,
                                       NULL,
                                       GSD_USB_PROTECTION_DBUS_PATH,
                                       "org.freedesktop.DBus.Properties",
                                       "PropertiesChanged",
                                       props_changed, NULL);
}

static void
on_usb_protection_owner_changed_cb (GObject    *object,
                                    GParamSpec *pspec,
                                    gpointer    user_data)
{
        GsdUsbProtectionManager *manager = user_data;
        GDBusProxy *proxy = G_DBUS_PROXY(object);
        g_autofree gchar *name_owner = NULL;

        name_owner = g_dbus_proxy_get_name_owner (proxy);
        g_debug ("Got owner change: %s", name_owner);

        if (name_owner) {
                manager->available = TRUE;
        } else {
                manager->available = FALSE;
        }

        usb_protection_properties_changed (manager);
}

static void
handle_screensaver_active (GsdUsbProtectionManager *manager,
                           GVariant                *parameters)
{
        gboolean active;
        gchar *value_usbguard;
        gboolean usbguard_controlled;
        GVariant *params;
        GDesktopUsbProtection protection_level;
        GSettings *settings = manager->settings;

        usbguard_controlled = g_settings_get_boolean (settings, USB_PROTECTION);
        protection_level = g_settings_get_enum (settings, USB_PROTECTION_LEVEL);

        g_variant_get (parameters, "(b)", &active);
        g_debug ("Received screensaver ActiveChanged signal: %d (old: %d)", active, manager->screensaver_active);
        if (manager->screensaver_active != active) {
                manager->screensaver_active = active;
                if (usbguard_controlled && protection_level == G_DESKTOP_USB_PROTECTION_LOCKSCREEN) {
                        /* If we are in the "lockscreen protection" level we change
                         * the usbguard config with apply-policy or block if the session
                         * is unlocked or locked, respectively. */
                        value_usbguard = active ? BLOCK : APPLY_POLICY;
                        params = g_variant_new ("(ss)",
                                                INSERTED_DEVICE_POLICY,
                                                value_usbguard);
                        if (manager->usb_protection != NULL) {
                                g_dbus_proxy_call (manager->usb_protection,
                                                   "setParameter",
                                                   params,
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   -1,
                                                   manager->cancellable,
                                                   dbus_call_log_error,
                                                   "Error calling USBGuard DBus to change the protection after a screensaver event");
                        }
                }
        }
}

static void
screensaver_signal_cb (GDBusProxy  *proxy,
                       const gchar *sender_name,
                       const gchar *signal_name,
                       GVariant    *parameters,
                       gpointer     user_data)
{
        g_debug ("ScreenSaver Signal: %s", signal_name);
        if (g_strcmp0 (signal_name, "ActiveChanged") == 0)
                handle_screensaver_active (GSD_USB_PROTECTION_MANAGER (user_data), parameters);
}

static void
usb_protection_policy_proxy_ready (GObject      *source_object,
                                   GAsyncResult *res,
                                   gpointer      user_data)
{
        GsdUsbProtectionManager *manager;
        GDBusProxy *proxy;
        g_autoptr(GError) error = NULL;
        g_debug ("usb_protection_policy_proxy_ready");

        proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
        if (!proxy) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to contact USBGuard: %s", error->message);
                return;
        } else {
            manager = GSD_USB_PROTECTION_MANAGER (user_data);
            manager->usb_protection_policy = proxy;
            g_debug ("Set protection policy proxy to %p", proxy);
            sync_usb_protection (proxy, manager);
        }
}

static void
usb_protection_devices_proxy_ready (GObject      *source_object,
                                    GAsyncResult *res,
                                    gpointer      user_data)
{
        GsdUsbProtectionManager *manager;
        GDBusProxy *proxy;
        g_autoptr(GError) error = NULL;

        proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
        if (!proxy) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to contact USBGuard: %s", error->message);
                return;
        }
        manager = GSD_USB_PROTECTION_MANAGER (user_data);
        manager->usb_protection_devices = proxy;

        /* We don't care about already plugged in devices because they'll be
         * already autorized by the "allow all" rule in USBGuard. */
        g_debug ("Listening to signals");
        g_signal_connect_object (source_object,
                                 "g-signal",
                                 G_CALLBACK (on_usbguard_signal),
                                 user_data,
                                 0);
}

static void
get_current_screen_saver_status (GsdUsbProtectionManager *manager)
{
        g_autoptr(GVariant) ret = NULL;
        g_autoptr(GError) error = NULL;

        ret = g_dbus_proxy_call_sync (G_DBUS_PROXY (manager->screensaver_proxy),
                                      "GetActive",
                                      NULL,
                                      G_DBUS_CALL_FLAGS_NONE,
                                      -1,
                                      manager->cancellable,
                                      &error);
        if (ret == NULL) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to get screen saver status: %s", error->message);
                return;
        }
        handle_screensaver_active (manager, ret);
}

static void
usb_protection_proxy_ready (GObject      *source_object,
                            GAsyncResult *res,
                            gpointer      user_data)
{
        GsdUsbProtectionManager *manager;
        GDBusProxy *proxy;
        g_autofree gchar *name_owner = NULL;
        g_autoptr(GError) error = NULL;

        proxy = g_dbus_proxy_new_for_bus_finish (res, &error);
        if (!proxy) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Failed to contact USBGuard: %s", error->message);
                return;
        }
        manager = GSD_USB_PROTECTION_MANAGER (user_data);
        manager->usb_protection = proxy;

        g_signal_connect (G_OBJECT (manager->settings), "changed",
                          G_CALLBACK (settings_changed_callback), manager);

        manager->screensaver_proxy = gnome_settings_bus_get_screen_saver_proxy ();

        get_current_screen_saver_status (manager);

        g_signal_connect (manager->screensaver_proxy, "g-signal",
                          G_CALLBACK (screensaver_signal_cb), manager);

        name_owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (proxy));

        if (name_owner == NULL) {
                g_debug("Probably USBGuard >= 0.7.5 is not currently installed.");
                manager->available = FALSE;
        } else {
                manager->available = TRUE;
        }

        g_signal_connect_object (source_object,
                                 "notify::g-name-owner",
                                 G_CALLBACK (on_usb_protection_owner_changed_cb),
                                 user_data,
                                 0);

        g_signal_connect_object (source_object,
                                 "g-signal",
                                 G_CALLBACK (on_usb_protection_signal),
                                 user_data,
                                 0);

        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                  G_DBUS_PROXY_FLAGS_NONE,
                                  NULL,
                                  USBGUARD_DBUS_NAME,
                                  USBGUARD_DBUS_PATH_DEVICES,
                                  USBGUARD_DBUS_INTERFACE_DEVICES,
                                  manager->cancellable,
                                  usb_protection_devices_proxy_ready,
                                  manager);

        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                  G_DBUS_PROXY_FLAGS_NONE,
                                  NULL,
                                  USBGUARD_DBUS_NAME,
                                  USBGUARD_DBUS_PATH_POLICY,
                                  USBGUARD_DBUS_INTERFACE_POLICY,
                                  manager->cancellable,
                                  usb_protection_policy_proxy_ready,
                                  manager);
}

static GVariant *
handle_get_property (GDBusConnection *connection,
                     const gchar     *sender,
                     const gchar     *object_path,
                     const gchar     *interface_name,
                     const gchar     *property_name,
                     GError         **error,
                     gpointer         user_data)
{
        GsdUsbProtectionManager *manager = GSD_USB_PROTECTION_MANAGER (user_data);

        /* Check session pointer as a proxy for whether the manager is in the
           start or stop state */
        if (manager->connection == NULL)
                return NULL;

        if (g_strcmp0 (property_name, "Available") == 0)
                return g_variant_new_boolean (manager->available);

        return NULL;
}

static const GDBusInterfaceVTable interface_vtable =
{
        NULL,
        handle_get_property,
        NULL
};

static void
on_bus_gotten (GObject                 *source_object,
               GAsyncResult            *res,
               GsdUsbProtectionManager *manager)
{
        GDBusConnection *connection;
        GError *error = NULL;

        connection = g_bus_get_finish (res, &error);
        if (connection == NULL) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        g_warning ("Could not get session bus: %s", error->message);
                g_error_free (error);
                return;
        }
        manager->connection = connection;

        g_dbus_connection_register_object (connection,
                                           GSD_USB_PROTECTION_DBUS_PATH,
                                           manager->introspection_data->interfaces[0],
                                           &interface_vtable,
                                           manager,
                                           NULL,
                                           NULL);

        manager->name_id = g_bus_own_name_on_connection (connection,
                                                         GSD_USB_PROTECTION_DBUS_NAME,
                                                         G_BUS_NAME_OWNER_FLAGS_NONE,
                                                         NULL,
                                                         NULL,
                                                         NULL,
                                                         NULL);
}

static gboolean
start_usb_protection_idle_cb (GsdUsbProtectionManager *manager)
{
        g_debug ("Starting USB protection manager");

        manager->settings = g_settings_new (PRIVACY_SETTINGS);
        manager->cancellable = g_cancellable_new ();

        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                  G_DBUS_PROXY_FLAGS_NONE,
                                  NULL,
                                  USBGUARD_DBUS_NAME,
                                  USBGUARD_DBUS_PATH,
                                  USBGUARD_DBUS_INTERFACE_VERSIONED,
                                  manager->cancellable,
                                  usb_protection_proxy_ready,
                                  manager);

        notify_init ("gnome-settings-daemon");

        manager->start_idle_id = 0;

        return FALSE;
}

gboolean
gsd_usb_protection_manager_start (GsdUsbProtectionManager *manager,
                                  GError                 **error)
{
        gnome_settings_profile_start (NULL);

        manager->start_idle_id = g_idle_add ((GSourceFunc) start_usb_protection_idle_cb, manager);
        g_source_set_name_by_id (manager->start_idle_id, "[gnome-settings-daemon] start_usbguard_idle_cb");

        manager->introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
        g_assert (manager->introspection_data != NULL);

        /* Start process of owning a D-Bus name */
        g_bus_get (G_BUS_TYPE_SESSION,
                   manager->cancellable,
                   (GAsyncReadyCallback) on_bus_gotten,
                   manager);

        gnome_settings_profile_end (NULL);
        return TRUE;
}

void
gsd_usb_protection_manager_stop (GsdUsbProtectionManager *manager)
{
        g_debug ("Stopping USB protection manager");

        if (manager->cancellable != NULL) {
                g_cancellable_cancel (manager->cancellable);
                g_clear_object (&manager->cancellable);
        }

        g_clear_object (&manager->notification);

        if (manager->start_idle_id != 0) {
                g_source_remove (manager->start_idle_id);
                manager->start_idle_id = 0;
        }

        if (manager->name_id != 0) {
                g_bus_unown_name (manager->name_id);
                manager->name_id = 0;
        }

        g_clear_pointer (&manager->introspection_data, g_dbus_node_info_unref);
        g_clear_object (&manager->connection);
        g_clear_object (&manager->settings);
        g_clear_object (&manager->usb_protection);
        g_clear_object (&manager->usb_protection_devices);
        g_clear_object (&manager->usb_protection_policy);
        g_clear_object (&manager->screensaver_proxy);
}

static void
gsd_usb_protection_manager_class_init (GsdUsbProtectionManagerClass *klass)
{
        GObjectClass *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = gsd_usb_protection_manager_finalize;
}

static void
gsd_usb_protection_manager_init (GsdUsbProtectionManager *manager)
{
}

static void
gsd_usb_protection_manager_finalize (GObject *object)
{
        GsdUsbProtectionManager *usb_protection_manager;

        usb_protection_manager = GSD_USB_PROTECTION_MANAGER (object);
        gsd_usb_protection_manager_stop (usb_protection_manager);

        G_OBJECT_CLASS (gsd_usb_protection_manager_parent_class)->finalize (object);
}

GsdUsbProtectionManager *
gsd_usb_protection_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (GSD_TYPE_USB_PROTECTION_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
        }

        return GSD_USB_PROTECTION_MANAGER (manager_object);
}
