/*
 * Copyright © 2014 Red Hat, Inc
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 */

#include "config.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <gio/gio.h>
#include <glib/gprintf.h>
#include <polkit/polkit.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pwd.h>
#include <gio/gunixfdlist.h>
#include <sys/mount.h>

#include "flatpak-dbus-generated.h"
#include "flatpak-dir-private.h"
#include "flatpak-oci-registry-private.h"
#include "flatpak-error.h"

static PolkitAuthority *authority = NULL;
static FlatpakSystemHelper *helper = NULL;
static GMainLoop *main_loop = NULL;
static guint name_owner_id = 0;

G_LOCK_DEFINE (ongoing_pulls);
static GPtrArray *ongoing_pulls = NULL;

static gboolean on_session_bus = FALSE;
static gboolean no_idle_exit = FALSE;

#define IDLE_TIMEOUT_SECS 10 * 60

/* This uses a weird Auto prefix to avoid conflicts with later added polkit types.
 */
typedef PolkitAuthorizationResult AutoPolkitAuthorizationResult;
typedef PolkitDetails             AutoPolkitDetails;
typedef PolkitSubject             AutoPolkitSubject;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitAuthorizationResult, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitDetails, g_object_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC (AutoPolkitSubject, g_object_unref)

typedef struct
{
  FlatpakSystemHelper *object;
  GDBusMethodInvocation *invocation;
  GCancellable *cancellable;

  guint watch_id;
  uid_t uid;

  gint ref_count;
  gint client_socket;

  gchar *ref;
  gchar *commit;

  gchar *src_dir;
  gchar *mountpoint;

  GSubprocess *revokefs_backend;
} OngoingPull;

static void
ongoing_pull_free (OngoingPull *pull)
{
  g_autoptr(GFile) src_dir_file = NULL;
  g_autoptr(GFile) mountpoint_file = NULL;
  g_autoptr(GError) local_error = NULL;

  if (pull->watch_id)
    g_clear_handle_id (&pull->watch_id, g_bus_unwatch_name);

  src_dir_file = g_file_new_for_path (pull->src_dir);
  mountpoint_file = g_file_new_for_path (pull->mountpoint);

  if (pull->revokefs_backend)
    {
      g_subprocess_force_exit (pull->revokefs_backend);
      g_clear_object (&pull->revokefs_backend);
    }

  if (!flatpak_rm_rf (src_dir_file, NULL, &local_error))
    {
      g_warning ("Unable to remove ongoing pull's src dir at: %s", local_error->message);
      g_clear_error (&local_error);
    }

   if (!flatpak_rm_rf (mountpoint_file, NULL, &local_error))
    {
      g_warning ("Unable to remove ongoing pull's mountpoint at: %s", local_error->message);
      g_clear_error (&local_error);
    }

  g_clear_pointer (&pull->src_dir, g_free);
  g_clear_pointer (&pull->mountpoint, g_free);
  g_clear_pointer (&pull->ref, g_free);
  g_clear_pointer (&pull->commit, g_free);

  g_slice_free (OngoingPull, pull);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (OngoingPull, ongoing_pull_free);

static void
skeleton_died_cb (gpointer data)
{
  g_debug ("skeleton finalized, exiting");
  g_main_loop_quit (main_loop);
}

static gboolean
unref_skeleton_in_timeout_cb (gpointer user_data)
{
  static gboolean unreffed = FALSE;

  g_debug ("unreffing helper main ref");
  if (!unreffed)
    {
      g_object_unref (helper);
      unreffed = TRUE;
    }

  return G_SOURCE_REMOVE;
}

static void
unref_skeleton_in_timeout (void)
{
  if (name_owner_id)
    g_bus_unown_name (name_owner_id);
  name_owner_id = 0;

  /* After we've lost the name or idled we drop the main ref on the helper
     so that we'll exit when it drops to zero. However, if there are
     outstanding calls these will keep the refcount up during the
     execution of them. We do the unref on a timeout to make sure
     we're completely draining the queue of (stale) requests. */
  g_timeout_add (500, unref_skeleton_in_timeout_cb, NULL);
}

static gboolean
idle_timeout_cb (gpointer user_data)
{
  G_LOCK (ongoing_pulls);
  guint ongoing_pulls_len = ongoing_pulls->len;
  G_UNLOCK (ongoing_pulls);
  if (ongoing_pulls_len != 0)
    return G_SOURCE_CONTINUE;

  if (name_owner_id)
    {
      g_debug ("Idle - unowning name");
      unref_skeleton_in_timeout ();
    }
  return G_SOURCE_REMOVE;
}

G_LOCK_DEFINE_STATIC (idle);
static void
schedule_idle_callback (void)
{
  static guint idle_timeout_id = 0;

  G_LOCK (idle);

  if (!no_idle_exit)
    {
      if (idle_timeout_id != 0)
        g_source_remove (idle_timeout_id);

      idle_timeout_id = g_timeout_add_seconds (IDLE_TIMEOUT_SECS, idle_timeout_cb, NULL);
    }

  G_UNLOCK (idle);
}

static FlatpakDir *
dir_get_system (const char *installation,
                pid_t       source_pid,
                GError **error)
{
  FlatpakDir *system = NULL;

  if (installation != NULL && *installation != '\0')
    system = flatpak_dir_get_system_by_id (installation, NULL, error);
  else
    system = flatpak_dir_get_system_default ();

  /* This can happen in case of error with flatpak_dir_get_system_by_id(). */
  if (system == NULL)
    return NULL;

  flatpak_dir_set_source_pid (system, source_pid);
  flatpak_dir_set_no_system_helper (system, TRUE);

  return system;
}

static void
no_progress_cb (OstreeAsyncProgress *progress, gpointer user_data)
{
}

#define DBUS_NAME_DBUS "org.freedesktop.DBus"
#define DBUS_INTERFACE_DBUS DBUS_NAME_DBUS
#define DBUS_PATH_DBUS "/org/freedesktop/DBus"

static int
get_sender_pid (GDBusMethodInvocation *invocation)
{
  g_autoptr(GDBusMessage) msg = NULL;
  g_autoptr(GDBusMessage) reply = NULL;
  GDBusConnection *connection;
  const char *sender;
  GVariant *body;
  g_autoptr(GVariantIter) iter = NULL;
  const char *key;
  g_autoptr(GVariant) value = NULL;

  connection = g_dbus_method_invocation_get_connection (invocation);
  sender = g_dbus_method_invocation_get_sender (invocation);

  msg = g_dbus_message_new_method_call (DBUS_NAME_DBUS,
                                        DBUS_PATH_DBUS,
                                        DBUS_INTERFACE_DBUS,
                                        "GetConnectionCredentials");
  g_dbus_message_set_body (msg, g_variant_new ("(s)", sender));

  reply = g_dbus_connection_send_message_with_reply_sync (connection, msg,
                                                          G_DBUS_SEND_MESSAGE_FLAGS_NONE,
                                                          30000,
                                                          NULL,
                                                          NULL,
                                                          NULL);
  if (reply == NULL)
    return 0;

  if (g_dbus_message_get_message_type (reply) == G_DBUS_MESSAGE_TYPE_ERROR)
    return 0;

  body = g_dbus_message_get_body (reply);

  g_variant_get (body, "(a{sv})", &iter);
  while (g_variant_iter_loop (iter, "{&sv}", &key, &value))
    {
      if (strcmp (key, "ProcessID") == 0)
        return g_variant_get_uint32 (value);
    }
  value = NULL; /* g_variant_iter_loop freed it */

  return 0;
}

static void
flatpak_invocation_return_error (GDBusMethodInvocation *invocation,
                                 GError                *error,
                                 const char            *fmt,
                                 ...)
{
  if (error->domain == FLATPAK_ERROR)
    g_dbus_method_invocation_return_gerror (invocation, error);
  else
    {
      va_list args;
      g_autofree char *prefix = NULL;
      va_start (args, fmt);
      g_vasprintf (&prefix, fmt, args);
      va_end (args);
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "%s: %s", prefix, error->message);
    }
}

static OngoingPull *
take_ongoing_pull_by_dir (const gchar *arg_repo_path)
{
  OngoingPull *pull = NULL;
  g_autofree gchar *dir = g_path_get_dirname (arg_repo_path);

  G_LOCK (ongoing_pulls);
  for (guint i = 0; i < ongoing_pulls->len; i++)
    {
      pull = g_ptr_array_index (ongoing_pulls, i);
      if (g_strcmp0 (pull->mountpoint, dir) == 0)
        break;
    }
  g_ptr_array_remove (ongoing_pulls, pull);
  G_UNLOCK (ongoing_pulls);

  return g_steal_pointer (&pull);
}

static gboolean
handle_deploy (FlatpakSystemHelper   *object,
               GDBusMethodInvocation *invocation,
               const gchar           *arg_repo_path,
               guint32                arg_flags,
               const gchar           *arg_ref,
               const gchar           *arg_origin,
               const gchar *const    *arg_subpaths,
               const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GFile) repo_file = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) deploy_dir = NULL;
  g_autoptr(OstreeAsyncProgress) ostree_progress = NULL;
  gboolean is_oci;
  gboolean no_deploy;
  gboolean local_pull;
  gboolean reinstall;
  g_autofree char *url = NULL;
  g_autoptr(OngoingPull) ongoing_pull = NULL;
  g_autofree gchar *repo_path = NULL;
  gint dir_fd = -1;

  ongoing_pull = take_ongoing_pull_by_dir (arg_repo_path);
  if (ongoing_pull != NULL)
    {
      g_autofree gchar *repo_basename = NULL;
      g_autoptr(GError) local_error = NULL;

      /* Killing the fuse backend will revoke all access to write operations. */
      g_subprocess_force_exit (ongoing_pull->revokefs_backend);
      g_clear_object (&ongoing_pull->revokefs_backend);

      repo_basename = g_path_get_basename (arg_repo_path);
      repo_path = g_build_filename (ongoing_pull->src_dir, repo_basename, NULL);
      repo_file = g_file_new_for_path (repo_path);

      dir_fd = open (ongoing_pull->src_dir, O_DIRECTORY);
      if (!flatpak_canonicalize_permissions (dir_fd,
                                             repo_path,
                                             getuid() == 0 ? 0 : -1,
                                             getuid() == 0 ? 0 : -1,
                                             &local_error))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Failed to canoncalize permissions: %s", local_error->message);
          return TRUE;
        }

    }
  else
    {
      repo_file = g_file_new_for_path (arg_repo_path);
      repo_path = g_strdup (arg_repo_path);
    }

  g_debug ("Deploy %s %u %s %s %s", repo_path, arg_flags, arg_ref, arg_origin, arg_installation);
  system = dir_get_system ("", get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_DEPLOY_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_DEPLOY_FLAGS_ALL));
      return TRUE;
    }

  if (!g_file_query_exists (repo_file, NULL))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Path does not exist");
      return TRUE;
    }

  no_deploy = (arg_flags & FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY) != 0;
  local_pull = (arg_flags & FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL) != 0;
  reinstall = (arg_flags & FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL) != 0;

  deploy_dir = flatpak_dir_get_if_deployed (system, arg_ref, NULL, NULL);

  if (deploy_dir && !reinstall)
    {
      g_autofree char *real_origin = NULL;
      real_origin = flatpak_dir_get_origin (system, arg_ref, NULL, NULL);
      if (g_strcmp0 (real_origin, arg_origin) != 0)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "Wrong origin %s for update", arg_origin);
          return TRUE;
        }
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Can't open system repo %s", arg_installation);
      return TRUE;
    }

  is_oci = flatpak_dir_get_remote_oci (system, arg_origin);

  if (strlen (repo_path) > 0 && is_oci)
    {
      g_autoptr(GFile) registry_file = g_file_new_for_path (repo_path);
      g_autofree char *registry_uri = g_file_get_uri (registry_file);
      g_autoptr(FlatpakOciRegistry) registry = NULL;
      g_autoptr(FlatpakOciIndex) index = NULL;
      const FlatpakOciManifestDescriptor *desc;
      g_autoptr(FlatpakOciVersioned) versioned = NULL;
      g_autoptr(FlatpakRemoteState) state = NULL;
      FlatpakCollectionRef collection_ref;
      g_autoptr(GHashTable) remote_refs = NULL;
      g_autofree char *checksum = NULL;
      const char *verified_digest;
      g_autofree char *upstream_url = NULL;

      ostree_repo_remote_get_url (flatpak_dir_get_repo (system),
                                  arg_origin,
                                  &upstream_url,
                                  NULL);

      if (upstream_url == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Remote %s is disabled", arg_origin);
          return TRUE;
        }

      registry = flatpak_oci_registry_new (registry_uri, FALSE, -1, NULL, &error);
      if (registry == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Can't open child OCI registry: %s", error->message);
          return TRUE;
        }

      index = flatpak_oci_registry_load_index (registry, NULL, &error);
      if (index == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Can't open child OCI registry index: %s", error->message);
          return TRUE;
        }

      desc = flatpak_oci_index_get_manifest (index, arg_ref);
      if (desc == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Can't find ref %s in child OCI registry index", arg_ref);
          return TRUE;
        }

      versioned = flatpak_oci_registry_load_versioned (registry, NULL, desc->parent.digest, NULL,
                                                       NULL, &error);
      if (versioned == NULL || !FLATPAK_IS_OCI_MANIFEST (versioned))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Can't open child manifest");
          return TRUE;
        }

      state = flatpak_dir_get_remote_state (system, arg_origin, NULL, &error);
      if (state == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "%s: Can't get remote state: %s", arg_origin, error->message);
          return TRUE;
        }

      /* We need to use list_all_remote_refs because we don't care about
       * enumerate vs. noenumerate.
       */
      if (!flatpak_dir_list_all_remote_refs (system, state, &remote_refs, NULL, &error))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "%s: Can't list refs: %s", arg_origin, error->message);
          return TRUE;
        }

      collection_ref.collection_id = state->collection_id;
      collection_ref.ref_name = (char *) arg_ref;

      verified_digest = g_hash_table_lookup (remote_refs, &collection_ref);
      if (!verified_digest)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "%s: ref %s not found", arg_origin, arg_ref);
          return TRUE;
        }

      if (!g_str_has_prefix (desc->parent.digest, "sha256:") ||
          strcmp (desc->parent.digest + strlen ("sha256:"), verified_digest) != 0)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "%s: manifest hash in downloaded content does not match ref %s", arg_origin, arg_ref);
          return TRUE;
        }

      checksum = flatpak_pull_from_oci (flatpak_dir_get_repo (system), registry, NULL, desc->parent.digest, FLATPAK_OCI_MANIFEST (versioned),
                                        arg_origin, arg_ref, NULL, NULL, NULL, &error);
      if (checksum == NULL)
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Can't pull ref %s from child OCI registry index: %s", arg_ref, error->message);
          return TRUE;
        }
    }
  else if (strlen (repo_path) > 0)
    {
      g_autoptr(GMainContextPopDefault) main_context = NULL;

      /* Work around ostree-pull spinning the default main context for the sync calls */
      main_context = flatpak_main_context_new_default ();

      ostree_progress = ostree_async_progress_new_and_connect (no_progress_cb, NULL);

      if (!flatpak_dir_pull_untrusted_local (system, repo_path,
                                             arg_origin,
                                             arg_ref,
                                             (const char **) arg_subpaths,
                                             ostree_progress,
                                             NULL, &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error pulling from repo");
          return TRUE;
        }

      if (ostree_progress)
        ostree_async_progress_finish (ostree_progress);
    }
  else if (local_pull)
    {
      g_autoptr(GMainContextPopDefault) main_context = NULL;

      g_autoptr(FlatpakRemoteState) state = NULL;
      if (!ostree_repo_remote_get_url (flatpak_dir_get_repo (system),
                                       arg_origin,
                                       &url,
                                       &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error getting remote url");
          return TRUE;
        }

      if (!g_str_has_prefix (url, "file:"))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Local pull url doesn't start with file://");
          return TRUE;
        }

      state = flatpak_dir_get_remote_state_optional (system, arg_origin, NULL, &error);
      if (state == NULL)
        {
          flatpak_invocation_return_error (invocation, error, "Error getting remote state");
          return TRUE;
        }

      /* Work around ostree-pull spinning the default main context for the sync calls */
      main_context = flatpak_main_context_new_default ();

      ostree_progress = ostree_async_progress_new_and_connect (no_progress_cb, NULL);

      if (!flatpak_dir_pull (system, state, arg_ref, NULL, NULL, (const char **) arg_subpaths, NULL,
                             FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_UNTRUSTED, ostree_progress,
                             NULL, &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error pulling from repo");
          return TRUE;
        }

      if (ostree_progress)
        ostree_async_progress_finish (ostree_progress);
    }

  if (!no_deploy)
    {
      if (deploy_dir && !reinstall)
        {
          if (!flatpak_dir_deploy_update (system, arg_ref,
                                          NULL, (const char **) arg_subpaths, NULL, &error))
            {
              flatpak_invocation_return_error (invocation, error, "Error deploying");
              return TRUE;
            }
        }
      else
        {
          if (!flatpak_dir_deploy_install (system, arg_ref, arg_origin,
                                           (const char **) arg_subpaths,
                                           reinstall,
                                           NULL, &error))
            {
              flatpak_invocation_return_error (invocation, error, "Error deploying");
              return TRUE;
            }
        }
    }

  flatpak_system_helper_complete_deploy (object, invocation);

  return TRUE;
}

static gboolean
handle_deploy_appstream (FlatpakSystemHelper   *object,
                         GDBusMethodInvocation *invocation,
                         const gchar           *arg_repo_path,
                         guint                  arg_flags,
                         const gchar           *arg_origin,
                         const gchar           *arg_arch,
                         const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *new_branch = NULL;
  g_autofree char *old_branch = NULL;
  gboolean is_oci;

  g_debug ("DeployAppstream %s %u %s %s %s", arg_repo_path, arg_flags, arg_origin, arg_arch, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (strlen (arg_repo_path) > 0)
    {
      g_autoptr(GFile) repo_file = g_file_new_for_path (arg_repo_path);
      if (!g_file_query_exists (repo_file, NULL))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                                 "Path does not exist");
          return TRUE;
        }
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Can't open system repo %s", error->message);
      return TRUE;
    }

  is_oci = flatpak_dir_get_remote_oci (system, arg_origin);

  new_branch = g_strdup_printf ("appstream2/%s", arg_arch);
  old_branch = g_strdup_printf ("appstream/%s", arg_arch);

  if (is_oci)
    {
      g_autoptr(GMainContextPopDefault) context =  NULL;

      /* This does soup http requests spinning the current mainloop, so we need one
         for this thread. */
      context = flatpak_main_context_new_default ();
      /* In the OCI case, we just do the full update, including network i/o, in the
       * system helper, see comment in flatpak_dir_update_appstream()
       */
      if (!flatpak_dir_update_appstream (system,
                                         arg_origin,
                                         arg_arch,
                                         NULL,
                                         NULL,
                                         NULL,
                                         &error))
        { 
          flatpak_invocation_return_error (invocation, error, "Error updating appstream");
          return TRUE;
        }

      flatpak_system_helper_complete_deploy_appstream (object, invocation);
      return TRUE;
    }
  else if (strlen (arg_repo_path) > 0)
    {
      g_autoptr(GError) first_error = NULL;
      g_autoptr(GError) second_error = NULL;
      g_autoptr(GMainContextPopDefault) main_context = NULL;
      g_autoptr(OstreeAsyncProgress) ostree_progress = NULL;

      /* Work around ostree-pull spinning the default main context for the sync calls */
      main_context = flatpak_main_context_new_default ();

      ostree_progress = ostree_async_progress_new_and_connect (no_progress_cb, NULL);

      if (!flatpak_dir_pull_untrusted_local (system, arg_repo_path,
                                             arg_origin,
                                             new_branch,
                                             NULL,
                                             ostree_progress,
                                             NULL, &first_error))
        {
          if (!flatpak_dir_pull_untrusted_local (system, arg_repo_path,
                                                 arg_origin,
                                                 old_branch,
                                                 NULL,
                                                 ostree_progress,
                                                 NULL, &second_error))
            {
              g_prefix_error (&first_error, "Error updating appstream2: ");
              g_prefix_error (&second_error, "%s; Error updating appstream: ", first_error->message);
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                     "Error pulling from repo: %s", second_error->message);
              return TRUE;
            }
        }
    }
  else /* empty path == local pull */
    {
      g_autoptr(FlatpakRemoteState) state = NULL;
      g_autoptr(OstreeAsyncProgress) ostree_progress = NULL;
      g_autoptr(GError) first_error = NULL;
      g_autoptr(GError) second_error = NULL;
      g_autofree char *url = NULL;
      g_autoptr(GMainContextPopDefault) main_context = NULL;

      if (!ostree_repo_remote_get_url (flatpak_dir_get_repo (system),
                                       arg_origin,
                                       &url,
                                       &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error getting remote url");
          return TRUE;
        }

      if (!g_str_has_prefix (url, "file:"))
        {
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Local pull url doesn't start with file://");
          return TRUE;
        }

      state = flatpak_dir_get_remote_state_optional (system, arg_origin, NULL, &error);
      if (state == NULL)
        {
          flatpak_invocation_return_error (invocation, error, "Error getting remote state");
          return TRUE;
        }

      /* Work around ostree-pull spinning the default main context for the sync calls */
      main_context = flatpak_main_context_new_default ();

      ostree_progress = ostree_async_progress_new_and_connect (no_progress_cb, NULL);

      if (!flatpak_dir_pull (system, state, new_branch, NULL, NULL, NULL, NULL,
                             FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_UNTRUSTED, ostree_progress,
                             NULL, &first_error))
        {
          if (!flatpak_dir_pull (system, state, old_branch, NULL, NULL, NULL, NULL,
                                 FLATPAK_PULL_FLAGS_NONE, OSTREE_REPO_PULL_FLAGS_UNTRUSTED, ostree_progress,
                                 NULL, &second_error))
            {
              g_prefix_error (&first_error, "Error updating appstream2: ");
              g_prefix_error (&second_error, "%s; Error updating appstream: ", first_error->message);
              g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                     "Error pulling from repo: %s", second_error->message);
              return TRUE;
            }
        }

      if (ostree_progress)
        ostree_async_progress_finish (ostree_progress);
    }

  if (!flatpak_dir_deploy_appstream (system,
                                     arg_origin,
                                     arg_arch,
                                     NULL,
                                     NULL,
                                     &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error deploying appstream");
      return TRUE;
    }

  flatpak_system_helper_complete_deploy_appstream (object, invocation);

  return TRUE;
}

static gboolean
handle_uninstall (FlatpakSystemHelper   *object,
                  GDBusMethodInvocation *invocation,
                  guint                  arg_flags,
                  const gchar           *arg_ref,
                  const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("Uninstall %u %s %s", arg_flags, arg_ref, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_UNINSTALL_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_UNINSTALL_FLAGS_ALL));
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_uninstall (system, arg_ref, arg_flags, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error uninstalling");
      return TRUE;
    }

  flatpak_system_helper_complete_uninstall (object, invocation);

  return TRUE;
}

static gboolean
handle_install_bundle (FlatpakSystemHelper   *object,
                       GDBusMethodInvocation *invocation,
                       const gchar           *arg_bundle_path,
                       guint32                arg_flags,
                       const gchar           *arg_remote,
                       const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GFile) bundle_file = g_file_new_for_path (arg_bundle_path);
  g_autoptr(GError) error = NULL;
  g_autofree char *ref = NULL;

  g_debug ("InstallBundle %s %u %s %s", arg_bundle_path, arg_flags, arg_remote, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_ALL));
      return TRUE;
    }

  if (!g_file_query_exists (bundle_file, NULL))
    {
      g_dbus_method_invocation_return_error (invocation, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                             "Bundle %s does not exist", arg_bundle_path);
      return TRUE;
    }

  if (!flatpak_dir_install_bundle (system, bundle_file, arg_remote, &ref, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error installing bundle");
      return TRUE;
    }

  flatpak_system_helper_complete_install_bundle (object, invocation, ref);

  return TRUE;
}


static gboolean
handle_configure_remote (FlatpakSystemHelper   *object,
                         GDBusMethodInvocation *invocation,
                         guint                  arg_flags,
                         const gchar           *arg_remote,
                         const gchar           *arg_config,
                         GVariant              *arg_gpg_key,
                         const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) config = g_key_file_new ();
  g_autofree char *group = g_strdup_printf ("remote \"%s\"", arg_remote);
  g_autoptr(GBytes) gpg_data = NULL;
  gboolean force_remove;

  g_debug ("ConfigureRemote %u %s %s", arg_flags, arg_remote, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (*arg_remote == 0 || strchr (arg_remote, '/') != NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid remote name: %s", arg_remote);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_ALL));
      return TRUE;
    }

  if (!g_key_file_load_from_data (config, arg_config, strlen (arg_config),
                                  G_KEY_FILE_NONE, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid config: %s\n", error->message);
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (g_variant_get_size (arg_gpg_key) > 0)
    gpg_data = g_variant_get_data_as_bytes (arg_gpg_key);

  force_remove = (arg_flags & FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE) != 0;

  if (g_key_file_has_group (config, group))
    {
      if (!flatpak_dir_modify_remote (system, arg_remote, config, gpg_data, NULL, &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error modifying remote");
          return TRUE;
        }
    }
  else
    {
      if (!flatpak_dir_remove_remote (system, force_remove, arg_remote, NULL, &error))
        {
          flatpak_invocation_return_error (invocation, error, "Error removing remote");
          return TRUE;
        }
    }

  flatpak_system_helper_complete_configure_remote (object, invocation);

  return TRUE;
}

static gboolean
handle_configure (FlatpakSystemHelper   *object,
                  GDBusMethodInvocation *invocation,
                  guint                  arg_flags,
                  const gchar           *arg_key,
                  const gchar           *arg_value,
                  const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("Configure %u %s=%s %s", arg_flags, arg_key, arg_value, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_CONFIGURE_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_CONFIGURE_FLAGS_ALL));
      return TRUE;
    }

  /* We only support this for now */
  if (strcmp (arg_key, "languages") != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported key: %s", arg_key);
      return TRUE;
    }

  if ((arg_flags & FLATPAK_HELPER_CONFIGURE_FLAGS_UNSET) != 0)
    arg_value = NULL;

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_set_config (system, arg_key, arg_value, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error setting config");
      return TRUE;
    }

  flatpak_system_helper_complete_configure (object, invocation);

  return TRUE;
}

static gboolean
handle_update_remote (FlatpakSystemHelper   *object,
                      GDBusMethodInvocation *invocation,
                      guint                  arg_flags,
                      const gchar           *arg_remote,
                      const gchar           *arg_installation,
                      const gchar           *arg_summary_path,
                      const gchar           *arg_summary_sig_path)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;
  char *summary_data = NULL;
  gsize summary_size;
  g_autoptr(GBytes) summary_bytes = NULL;
  char *summary_sig_data = NULL;
  gsize summary_sig_size;
  g_autoptr(GBytes) summary_sig_bytes = NULL;
  g_autoptr(FlatpakRemoteState) state = NULL;

  g_debug ("UpdateRemote %u %s %s %s %s", arg_flags, arg_remote, arg_installation, arg_summary_path, arg_summary_sig_path);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (*arg_remote == 0 || strchr (arg_remote, '/') != NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid remote name: %s", arg_remote);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_ALL));
      return TRUE;
    }

  if (!g_file_get_contents (arg_summary_path, &summary_data, &summary_size, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  summary_bytes = g_bytes_new_take (summary_data, summary_size);

  if (*arg_summary_sig_path != 0)
    {
      if (!g_file_get_contents (arg_summary_sig_path, &summary_sig_data, &summary_sig_size, &error))
        {
          g_dbus_method_invocation_return_gerror (invocation, error);
          return TRUE;
        }
      summary_sig_bytes = g_bytes_new_take (summary_sig_data, summary_sig_size);
    }

  state = flatpak_dir_get_remote_state_for_summary (system, arg_remote, summary_bytes, summary_sig_bytes, NULL, &error);
  if (state == NULL)
    {
      flatpak_invocation_return_error (invocation, error, "Error getting remote state");
      return TRUE;
    }

  if (summary_sig_bytes == NULL && state->collection_id == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "UpdateRemote requires a summary signature");
      return TRUE;
    }

  if (!flatpak_dir_update_remote_configuration_for_state (system, state, FALSE, NULL, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error updating remote config");
      return TRUE;
    }

  flatpak_system_helper_complete_update_remote (object, invocation);

  return TRUE;
}

static gboolean
handle_remove_local_ref (FlatpakSystemHelper   *object,
                         GDBusMethodInvocation *invocation,
                         guint                  arg_flags,
                         const gchar           *arg_remote,
                         const gchar           *arg_ref,
                         const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("RemoveLocalRef %u %s %s %s", arg_flags, arg_remote, arg_ref, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_REMOVE_LOCAL_REF_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_REMOVE_LOCAL_REF_FLAGS_ALL));
      return TRUE;
    }

  if (*arg_remote == 0 || strchr (arg_remote, '/') != NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Invalid remote name: %s", arg_remote);
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_remove_ref (system, arg_remote, arg_ref, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error removing ref");
      return TRUE;
    }

  flatpak_system_helper_complete_remove_local_ref (object, invocation);

  return TRUE;
}

static gboolean
handle_prune_local_repo (FlatpakSystemHelper   *object,
                         GDBusMethodInvocation *invocation,
                         guint                  arg_flags,
                         const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("PruneLocalRepo %u %s", arg_flags, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_PRUNE_LOCAL_REPO_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_PRUNE_LOCAL_REPO_FLAGS_ALL));
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_prune (system, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error pruning repo");
      return TRUE;
    }

  flatpak_system_helper_complete_prune_local_repo (object, invocation);

  return TRUE;
}


static gboolean
handle_ensure_repo (FlatpakSystemHelper   *object,
                    GDBusMethodInvocation *invocation,
                    guint                  arg_flags,
                    const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("EnsureRepo %u %s", arg_flags, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_ENSURE_REPO_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_ENSURE_REPO_FLAGS_ALL));
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  flatpak_system_helper_complete_ensure_repo (object, invocation);

  return TRUE;
}

static gboolean
handle_run_triggers (FlatpakSystemHelper   *object,
                     GDBusMethodInvocation *invocation,
                     guint                  arg_flags,
                     const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("RunTriggers %u %s", arg_flags, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_RUN_TRIGGERS_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_RUN_TRIGGERS_FLAGS_ALL));
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_run_triggers (system, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error running triggers");
      return TRUE;
    }

  flatpak_system_helper_complete_run_triggers (object, invocation);

  return TRUE;
}

static gboolean
get_connection_uid (GDBusMethodInvocation *invocation, uid_t *uid_out, GError **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  g_autoptr(GVariant) dict = NULL;
  g_autoptr(GVariant) unixUserId = NULL;
  g_autoptr(GVariant) credentials = NULL;
  uid_t uid;

   credentials = g_dbus_connection_call_sync (connection,
                                             "org.freedesktop.DBus",
                                             "/org/freedesktop/DBus",
                                             "org.freedesktop.DBus",
                                             "GetConnectionCredentials",
                                             g_variant_new ("(s)", sender),
                                             G_VARIANT_TYPE ("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, G_MAXINT, NULL, error);
  if (credentials == NULL)
    return FALSE;

  dict = g_variant_get_child_value (credentials, 0);

   if (!g_variant_lookup (dict, "UnixUserID", "u", uid_out))
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                   "Failed to query UnixUserId for the bus name: %s", sender);
      return FALSE;
    }
  g_print ("the UID: %d from get_connection_uid\n", *uid_out);

  return TRUE;
}


static void
revokefs_child_setup (gpointer user_data)
{
  struct passwd *passwd = getpwnam("flatpak");
  setgid (passwd->pw_gid);
  setuid (passwd->pw_uid);

  close (GPOINTER_TO_INT(user_data));
}

static void
name_vanished_cb (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  OngoingPull *pull = (OngoingPull *) user_data;

  ongoing_pull_free (pull);
}

static OngoingPull *
ongoing_pull_new (FlatpakSystemHelper   *object,
                  GDBusMethodInvocation *invocation,
                  const gchar           *ref,
                  const gchar           *commit,
                  uid_t                  uid,
                  gchar                 *revokefs_fuse_path,
                  gchar                 *src,
                  gchar                 *mnt,
                  GError               **error)
{
  GDBusConnection *connection = g_dbus_method_invocation_get_connection (invocation);
  OngoingPull *pull;
  g_autoptr (GSubprocessLauncher) launcher = NULL;
  g_autoptr (GSubprocess) revokefs_backend = NULL;
  g_autofree gchar *target_socket = NULL;
  int sockets[2];

  pull = g_slice_new0 (OngoingPull);
  pull->object = object;
  pull->invocation = invocation;
  pull->ref = g_strdup (ref);
  pull->commit = g_strdup (commit);
  pull->src_dir = g_strdup (src);
  pull->mountpoint = g_strdup (mnt);
  pull->cancellable = g_cancellable_new ();
  pull->uid = uid;

  pull->watch_id = g_bus_watch_name_on_connection (connection,
                                                   g_dbus_connection_get_unique_name (connection),
                                                   G_BUS_NAME_WATCHER_FLAGS_NONE, NULL,
                                                   name_vanished_cb,
                                                   pull, NULL); /* TODO: Take a ref here */

  if (socketpair (AF_UNIX, SOCK_SEQPACKET, 0, sockets) == -1)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "Syscall socketpair failed");
      return NULL;
    }

  launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_set_child_setup (launcher,
                                         revokefs_child_setup, GINT_TO_POINTER (sockets[1]),
                                         NULL);
  g_subprocess_launcher_take_fd (launcher, sockets[0], 3);
  target_socket = g_strdup ("--socket=3");
  pull->revokefs_backend = g_subprocess_launcher_spawn (launcher,
                                                        error,
                                                        revokefs_fuse_path,
                                                        "--backend",
                                                        target_socket, src, NULL);
  if (pull->revokefs_backend == NULL)
    {
      g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_SPAWN_FAILED,
                   "Could not spawn revokefs backend");
      return NULL;
    }

  pull->client_socket = sockets[1];
  return pull;
}

static gboolean
check_for_repo_containing_partial_pull (GFile *cache_dir, const gchar *commit, gchar **out_repo_location)
{
  g_autoptr(GFileEnumerator) enumerator = NULL;
  GFileInfo *file_info = NULL;
  g_autoptr(GError) error = NULL;
  const gchar *name;

  enumerator = g_file_enumerate_children (cache_dir,
                                          G_FILE_ATTRIBUTE_STANDARD_NAME,
                                          G_FILE_QUERY_INFO_NONE, NULL, &error);
  if (enumerator == NULL)
    {
      g_warning ("Failed to get file enumerator: %s", error->message);
      return FALSE;
    }

   while (TRUE)
    {
      if (!g_file_enumerator_iterate (enumerator, &file_info, NULL, NULL, &error))
        {
          g_warning ("Error while file enumerator iterating: %s", error->message);
          break;
        }

       if (file_info == NULL)
        break;

      name = g_file_info_get_name (file_info);
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY &&
          g_str_has_prefix (name, "repo-"))
        {
          g_autofree gchar *commit_partial_basename = g_strdup_printf ("%s.commitpartial", commit);
          g_autoptr(GFile) repo_file = g_file_get_child (cache_dir, name);
          g_autoptr(GFile) state = g_file_get_child (repo_file, "state");
          g_autoptr(GFile) commit_partial = g_file_get_child (state, commit_partial_basename);

           if (g_file_query_exists (commit_partial, NULL))
            {
              g_autoptr(OstreeRepo) repo = ostree_repo_new (repo_file);
              g_autofree gchar *repo_path = g_file_get_path (repo_file);
              /* Validate repo before returning */
              if (!ostree_repo_open (repo, NULL, &error))
                {
                  g_warning ("Partial pull available at %s, but invalid repo: %s", repo_path, error->message);
                  return FALSE;
                }
              else
                {
                  *out_repo_location = g_strdup (repo_path);
                  return TRUE;
                }
            }
        }
    }

   return FALSE;
}

static gboolean
check_partial_pull_availability (const gchar *repo_tmp,
                                 const gchar *ref_basename,
                                 const gchar *commit,
                                 gchar      **out_location)
{
  g_autoptr(GFileEnumerator) enumerator = NULL;
  g_autoptr(GFile) repo_tmpfile = NULL;
  GFileInfo *file_info = NULL;
  g_autofree gchar *prefix = NULL;
  g_autoptr(GError) error = NULL;
  const gchar *name;

  g_debug ("Checking partial pull availability for ref %s (commit: %s)", ref_basename, commit);

  repo_tmpfile = g_file_new_for_path (repo_tmp);
  enumerator = g_file_enumerate_children (repo_tmpfile,
                                          G_FILE_ATTRIBUTE_STANDARD_NAME,
                                          G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (enumerator == NULL)
    {
      g_warning ("Failed to get file enumerator: %s", error->message);
      return FALSE;
    }

   prefix = g_strdup_printf ("flatpak-cache-%s", ref_basename);

   /* We are looking for flatpak-cache-$ref-XXXXXX directory in repo/tmp.
      If we find one, we try to look for an OSTree repo which possibly has
      the $commit.commitpartial file denoting the partial pull for that $commit. */
  while (TRUE)
    {
      if (!g_file_enumerator_iterate (enumerator, &file_info, NULL, NULL, &error))
        {
          g_warning ("Error while file enumerator iterating: %s", error->message);
          break;
        }

      if (file_info == NULL)
        break;

      name = g_file_info_get_name (file_info);
      if (g_file_info_get_file_type (file_info) == G_FILE_TYPE_DIRECTORY &&
          g_str_has_prefix (name, prefix) &&
          !g_str_has_suffix (name, "mountpoint"))
        {
          g_autoptr(GFile) cache_dir = g_file_get_child (repo_tmpfile, name);
          OngoingPull *pull;

          /* If this cache directory is used by any other ongoing pull, we
           * should not be re(using) it. */
          G_LOCK (ongoing_pulls);
          for (guint i = 0; i < ongoing_pulls->len; i++)
            {
              pull = g_ptr_array_index (ongoing_pulls, i);
              if (g_strcmp0 (pull->src_dir, name) == 0)
                return FALSE;
            }
          G_UNLOCK (ongoing_pulls);

           if (check_for_repo_containing_partial_pull (cache_dir, commit, out_location))
            return TRUE;
        }
    }

  return FALSE;
}

static gboolean
handle_revokefs_fd (FlatpakSystemHelper   *object,
                    GDBusMethodInvocation *invocation,
                    GUnixFDList           *arg_fdlist,
                    guint                  arg_flags,
                    const gchar           *arg_ref,
                    const gchar           *arg_commit,
                    const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GFile) mnt_file = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GError) error = NULL;
  g_auto(GStrv) parts = NULL;
  g_autofree gchar *mnt = NULL;
  g_autofree gchar *src = NULL;
  g_autofree gchar *flatpak_dir = NULL;
  g_autofree gchar *repo_tmp = NULL;
  g_autofree gchar *cache_dir_basename = NULL;
  g_autofree gchar *revokefs_fuse_path = NULL;
  g_autofree gchar *partial_pull_repo = NULL;
  gboolean partial_pull_availablity = FALSE;
  struct passwd *passwd;
  OngoingPull *new_pull;
  uid_t uid;
  int fd_index = -1;
  int exit_status= -1;

  g_debug ("RevokefsFd %u %s %s %s", arg_flags, arg_ref, arg_commit, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  revokefs_fuse_path = g_find_program_in_path ("revokefs-fuse");
  if (revokefs_fuse_path == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
                                             "Can't find bindfs in the system.");
      return TRUE;
    }

  if (!get_connection_uid (invocation, &uid, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  flatpak_dir = g_file_get_path (flatpak_dir_get_path (system));
  repo_tmp = g_build_filename (flatpak_dir, "repo", "tmp", NULL);
  parts = flatpak_decompose_ref (arg_ref, NULL);

   if (check_partial_pull_availability (repo_tmp, parts[1], arg_commit, &partial_pull_repo))
    {
      g_debug ("Found a partial pull for ref %s at: %s", parts[1], partial_pull_repo);
      src = g_path_get_dirname (partial_pull_repo);
      partial_pull_availablity = TRUE;
    }
  else
    {
      /* Create backing directory and let it owned by "flatpak" user */
      cache_dir_basename = g_strdup_printf ("flatpak-cache-%s-XXXXXX", parts[1]);
      src = g_mkdtemp_full (g_build_filename (repo_tmp, cache_dir_basename, NULL), 0755);
      passwd = getpwnam ("flatpak");
      if (passwd == NULL)
        {
          glnx_throw_errno_prefix (&error, "Username flatpak does not exist");
          g_dbus_method_invocation_return_gerror (invocation, error);
          return TRUE;
        }
      chown (src, passwd->pw_uid, passwd->pw_gid);
    }

  /* Create mountpoint and let it owned by client */
  mnt = g_strdup_printf ("%s-mountpoint", src);
  mnt_file = g_file_new_for_path (mnt);
  passwd = getpwuid (uid);
  if (passwd == NULL)
    {
      glnx_throw_errno_prefix (&error, "UserID %d does not exist", uid);
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }
  if (!g_file_query_exists (mnt_file, NULL)) //and check if mnt_file is owned by user
    g_mkdir_with_parents (mnt, 0755);
  chown (mnt, passwd->pw_uid, passwd->pw_gid);

  new_pull = ongoing_pull_new (object, invocation, arg_ref, arg_commit,
                               uid, revokefs_fuse_path, src, mnt, &error);

  if (error != NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      ongoing_pull_free (new_pull);
      return TRUE;
    }

  G_LOCK (ongoing_pulls);
  g_ptr_array_add (ongoing_pulls, new_pull);
  G_UNLOCK (ongoing_pulls);

  fd_list = g_unix_fd_list_new ();
  fd_index = g_unix_fd_list_append (fd_list, new_pull->client_socket, NULL);

  flatpak_system_helper_complete_revokefs_fd (object, invocation,
                                              fd_list, g_variant_new_handle (fd_index),
                                              new_pull->src_dir,
                                              new_pull->mountpoint,
                                              partial_pull_availablity,
                                              partial_pull_repo);

  return TRUE;
}

static gboolean
handle_update_summary (FlatpakSystemHelper   *object,
                       GDBusMethodInvocation *invocation,
                       guint                  arg_flags,
                       const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;

  g_debug ("UpdateSummary %u %s", arg_flags, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_ALL));
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if (!flatpak_dir_update_summary (system, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Error updating summary");
      return TRUE;
    }

  flatpak_system_helper_complete_update_summary (object, invocation);

  return TRUE;
}

static gboolean
handle_generate_oci_summary (FlatpakSystemHelper   *object,
                             GDBusMethodInvocation *invocation,
                             guint                  arg_flags,
                             const gchar           *arg_origin,
                             const gchar           *arg_installation)
{
  g_autoptr(FlatpakDir) system = NULL;
  g_autoptr(GError) error = NULL;
  gboolean is_oci;

  g_debug ("GenerateOciSummary %u %s %s", arg_flags, arg_origin, arg_installation);

  system = dir_get_system (arg_installation, get_sender_pid (invocation), &error);
  if (system == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      return TRUE;
    }

  if ((arg_flags & ~FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_ALL) != 0)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "Unsupported flags enabled: 0x%x", (arg_flags & ~FLATPAK_HELPER_GENERATE_OCI_SUMMARY_FLAGS_ALL));
      return TRUE;
    }

  if (!flatpak_dir_ensure_repo (system, NULL, &error))
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                             "Can't open system repo %s", error->message);
      return TRUE;
    }

  is_oci = flatpak_dir_get_remote_oci (system, arg_origin);
  if (!is_oci)
    {
      g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                             "%s is not a OCI remote", arg_origin);
      return TRUE;
    }

  if (!flatpak_dir_remote_make_oci_summary (system, arg_origin, NULL, NULL, &error))
    {
      flatpak_invocation_return_error (invocation, error, "Failed to update OCI summary");
      return TRUE;
    }


  flatpak_system_helper_complete_generate_oci_summary (object, invocation);

  return TRUE;
}

static gboolean
dir_ref_is_installed (FlatpakDir *dir,
                      const char *ref)
{
  g_autoptr(GVariant) deploy_data = NULL;

  deploy_data = flatpak_dir_get_deploy_data (dir, ref, FLATPAK_DEPLOY_VERSION_ANY, NULL, NULL);

  return deploy_data != NULL;
}

static gboolean
flatpak_authorize_method_handler (GDBusInterfaceSkeleton *interface,
                                  GDBusMethodInvocation  *invocation,
                                  gpointer                user_data)
{
  const gchar *method_name = g_dbus_method_invocation_get_method_name (invocation);
  const gchar *sender = g_dbus_method_invocation_get_sender (invocation);
  GVariant *parameters = g_dbus_method_invocation_get_parameters (invocation);

  g_autoptr(AutoPolkitSubject) subject = polkit_system_bus_name_new (sender);
  g_autoptr(AutoPolkitDetails) details = polkit_details_new ();
  const gchar *action = NULL;
  gboolean authorized = FALSE;
  gboolean no_interaction = FALSE;

  /* Ensure we don't idle exit */
  schedule_idle_callback ();

  if (on_session_bus)
    {
      /* This is test code, make sure it never runs with privileges */
      g_assert (geteuid () != 0);
      g_assert (getuid () != 0);
      g_assert (getegid () != 0);
      g_assert (getgid () != 0);
      authorized = TRUE;
    }
  else if (g_strcmp0 (method_name, "Deploy") == 0)
    {
      const char *installation;
      const char *ref, *origin;
      guint32 flags;

      g_variant_get_child (parameters, 1, "u", &flags);
      g_variant_get_child (parameters, 2, "&s", &ref);
      g_variant_get_child (parameters, 3, "&s", &origin);
      g_variant_get_child (parameters, 5, "&s", &installation);

      /* For metadata updates, redirect to the metadata-update action which
       * should basically always be allowed */
      if (ref != NULL && g_strcmp0 (ref, OSTREE_REPO_METADATA_REF) == 0)
        {
          action = "org.freedesktop.Flatpak.metadata-update";
        }
      else
        {
          gboolean is_app, is_install;

          /* These flags allow clients to "upgrade" the permission,
           * avoiding the need for multiple polkit dialogs when we first
           * update a runtime, then install the app that needs it.
           *
           * Note that our policy has implications:
           * app-install > app-update > runtime-install > runtime-update
           * which means that these hints only ever select a stronger
           * permission, and are safe in that sense.
           */

          if ((flags & FLATPAK_HELPER_DEPLOY_FLAGS_APP_HINT) != 0)
            is_app = TRUE;
          else
            is_app = g_str_has_prefix (ref, "app/");

          if ((flags & FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT) != 0 ||
              (flags & FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL) != 0)
            is_install = TRUE;
          else
            {
              g_autoptr(FlatpakDir) system = dir_get_system (installation, 0, NULL);

              is_install = !dir_ref_is_installed (system, ref);
            }

          if (is_install)
            {
              if (is_app)
                action = "org.freedesktop.Flatpak.app-install";
              else
                action = "org.freedesktop.Flatpak.runtime-install";
            }
          else
            {
              if (is_app)
                action = "org.freedesktop.Flatpak.app-update";
              else
                action = "org.freedesktop.Flatpak.runtime-update";
            }

          no_interaction = (flags & FLATPAK_HELPER_DEPLOY_FLAGS_NO_INTERACTION) != 0;
        }

      polkit_details_insert (details, "origin", origin);
      polkit_details_insert (details, "ref", ref);
    }
  else if (g_strcmp0 (method_name, "DeployAppstream") == 0)
    {
      guint32 flags;
      const char *arch, *origin;

      g_variant_get_child (parameters, 1, "u", &flags);
      g_variant_get_child (parameters, 2, "&s", &origin);
      g_variant_get_child (parameters, 3, "&s", &arch);

      action = "org.freedesktop.Flatpak.appstream-update";
      no_interaction = (flags & FLATPAK_HELPER_DEPLOY_APPSTREAM_FLAGS_NO_INTERACTION) != 0;

      polkit_details_insert (details, "origin", origin);
      polkit_details_insert (details, "arch", arch);
    }
  else if (g_strcmp0 (method_name, "InstallBundle") == 0)
    {
      const char *path;
      guint32 flags;

      g_variant_get_child (parameters, 0, "^&ay", &path);
      g_variant_get_child (parameters, 1, "u", &flags);

      action = "org.freedesktop.Flatpak.install-bundle";
      no_interaction = (flags & FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_NO_INTERACTION) != 0;

      polkit_details_insert (details, "path", path);
    }
  else if (g_strcmp0 (method_name, "Uninstall") == 0)
    {
      const char *ref;
      gboolean is_app;
      guint32 flags;

      g_variant_get_child (parameters, 0, "u", &flags);
      g_variant_get_child (parameters, 1, "&s", &ref);

      is_app = g_str_has_prefix (ref, "app/");
      if (is_app)
        action = "org.freedesktop.Flatpak.app-uninstall";
      else
        action = "org.freedesktop.Flatpak.runtime-uninstall";
      no_interaction = (flags & FLATPAK_HELPER_UNINSTALL_FLAGS_NO_INTERACTION) != 0;

      polkit_details_insert (details, "ref", ref);
    }
  else if (g_strcmp0 (method_name, "ConfigureRemote") == 0)
    {
      const char *remote;
      guint32 flags;

      g_variant_get_child (parameters, 0, "u", &flags);
      g_variant_get_child (parameters, 1, "&s", &remote);

      action = "org.freedesktop.Flatpak.configure-remote";
      no_interaction = (flags & FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_NO_INTERACTION) != 0;

      polkit_details_insert (details, "remote", remote);
    }
  else if (g_strcmp0 (method_name, "Configure") == 0)
    {
      const char *key;
      guint32 flags;

      g_variant_get_child (parameters, 0, "u", &flags);
      g_variant_get_child (parameters, 1, "&s", &key);

      action = "org.freedesktop.Flatpak.configure";
      no_interaction = (flags & FLATPAK_HELPER_CONFIGURE_FLAGS_NO_INTERACTION) != 0;

      polkit_details_insert (details, "key", key);
    }
  else if (g_strcmp0 (method_name, "UpdateRemote") == 0)
    {
      const char *remote;
      guint32 flags;

      g_variant_get_child (parameters, 0, "u", &flags);
      g_variant_get_child (parameters, 1, "&s", &remote);

      action = "org.freedesktop.Flatpak.update-remote";
      no_interaction = (flags & FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_NO_INTERACTION) != 0;

      polkit_details_insert (details, "remote", remote);
    }
  else if (g_strcmp0 (method_name, "RemoveLocalRef") == 0 ||
           g_strcmp0 (method_name, "PruneLocalRepo") == 0 ||
           g_strcmp0 (method_name, "EnsureRepo") == 0 ||
           g_strcmp0 (method_name, "RunTriggers") == 0 ||
           g_strcmp0 (method_name, "RevokefsFd") == 0)
    {
      guint32 flags;

      action = "org.freedesktop.Flatpak.modify-repo";

      /* all of these methods have flags as first argument, and 1 << 0 as 'no-interaction' */
      g_variant_get_child (parameters, 0, "u", &flags);
      no_interaction = (flags & (1 << 0)) != 0;
    }
  else if (g_strcmp0 (method_name, "UpdateSummary") == 0 ||
           g_strcmp0 (method_name, "GenerateOciSummary") == 0)
    {
      action = "org.freedesktop.Flatpak.metadata-update";
    }

  if (action)
    {
      g_autoptr(AutoPolkitAuthorizationResult) result = NULL;
      g_autoptr(GError) error = NULL;
      PolkitCheckAuthorizationFlags auth_flags;

      if (no_interaction)
        auth_flags = POLKIT_CHECK_AUTHORIZATION_FLAGS_NONE;
      else
        auth_flags = POLKIT_CHECK_AUTHORIZATION_FLAGS_ALLOW_USER_INTERACTION;

      result = polkit_authority_check_authorization_sync (authority, subject,
                                                          action, details,
                                                          auth_flags,
                                                          NULL, &error);
      if (result == NULL)
        {
          g_dbus_error_strip_remote_error (error);
          g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                 "Authorization error: %s", error->message);
          return FALSE;
        }

      authorized = polkit_authorization_result_get_is_authorized (result);
    }

  if (!authorized)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             G_DBUS_ERROR,
                                             G_DBUS_ERROR_ACCESS_DENIED,
                                             "Flatpak system operation %s not allowed for user", method_name);
    }

  return authorized;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  GError *error = NULL;

  g_debug ("Bus acquired, creating skeleton");

  g_dbus_connection_set_exit_on_close (connection, FALSE);

  helper = flatpak_system_helper_skeleton_new ();

  flatpak_system_helper_set_version (FLATPAK_SYSTEM_HELPER (helper), 2);

  g_object_set_data_full (G_OBJECT (helper), "track-alive", GINT_TO_POINTER (42), skeleton_died_cb);

  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (helper),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

  g_signal_connect (helper, "handle-deploy", G_CALLBACK (handle_deploy), NULL);
  g_signal_connect (helper, "handle-deploy-appstream", G_CALLBACK (handle_deploy_appstream), NULL);
  g_signal_connect (helper, "handle-uninstall", G_CALLBACK (handle_uninstall), NULL);
  g_signal_connect (helper, "handle-install-bundle", G_CALLBACK (handle_install_bundle), NULL);
  g_signal_connect (helper, "handle-configure-remote", G_CALLBACK (handle_configure_remote), NULL);
  g_signal_connect (helper, "handle-configure", G_CALLBACK (handle_configure), NULL);
  g_signal_connect (helper, "handle-update-remote", G_CALLBACK (handle_update_remote), NULL);
  g_signal_connect (helper, "handle-remove-local-ref", G_CALLBACK (handle_remove_local_ref), NULL);
  g_signal_connect (helper, "handle-prune-local-repo", G_CALLBACK (handle_prune_local_repo), NULL);
  g_signal_connect (helper, "handle-ensure-repo", G_CALLBACK (handle_ensure_repo), NULL);
  g_signal_connect (helper, "handle-run-triggers", G_CALLBACK (handle_run_triggers), NULL);
  g_signal_connect (helper, "handle-update-summary", G_CALLBACK (handle_update_summary), NULL);
  g_signal_connect (helper, "handle-generate-oci-summary", G_CALLBACK (handle_generate_oci_summary), NULL);
  g_signal_connect (helper, "handle-revokefs-fd", G_CALLBACK (handle_revokefs_fd), NULL);

  g_signal_connect (helper, "g-authorize-method",
                    G_CALLBACK (flatpak_authorize_method_handler),
                    NULL);

  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (helper),
                                         connection,
                                         "/org/freedesktop/Flatpak/SystemHelper",
                                         &error))
    {
      g_warning ("error: %s", error->message);
      g_error_free (error);
    }
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_debug ("Name acquired");
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_debug ("Name lost");
  unref_skeleton_in_timeout ();
}

static void
binary_file_changed_cb (GFileMonitor     *file_monitor,
                        GFile            *file,
                        GFile            *other_file,
                        GFileMonitorEvent event_type,
                        gpointer          data)
{
  static gboolean got_it = FALSE;

  if (!got_it)
    {
      g_debug ("binary file changed");
      unref_skeleton_in_timeout ();
    }

  got_it = TRUE;
}

static void
message_handler (const gchar   *log_domain,
                 GLogLevelFlags log_level,
                 const gchar   *message,
                 gpointer       user_data)
{
  /* Make this look like normal console output */
  if (log_level & G_LOG_LEVEL_DEBUG)
    g_printerr ("F: %s\n", message);
  else
    g_printerr ("%s: %s\n", g_get_prgname (), message);
}

int
main (int    argc,
      char **argv)
{
  gchar exe_path[PATH_MAX + 1];
  ssize_t exe_path_len;
  gboolean replace;
  gboolean verbose;
  gboolean show_version;
  GBusNameOwnerFlags flags;
  GOptionContext *context;

  g_autoptr(GError) error = NULL;
  const GOptionEntry options[] = {
    { "replace", 'r', 0, G_OPTION_ARG_NONE, &replace,  "Replace old daemon.", NULL },
    { "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,  "Enable debug output.", NULL },
    { "session", 0, 0, G_OPTION_ARG_NONE, &on_session_bus,  "Run in session, not system scope (for tests).", NULL },
    { "no-idle-exit", 0, 0, G_OPTION_ARG_NONE, &no_idle_exit,  "Don't exit when idle.", NULL },
    { "version", 0, 0, G_OPTION_ARG_NONE, &show_version, "Show program version.", NULL},
    { NULL }
  };

  setlocale (LC_ALL, "");

  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_set_prgname (argv[0]);

  g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_MESSAGE, message_handler, NULL);

  context = g_option_context_new ("");

  replace = FALSE;
  verbose = FALSE;
  show_version = FALSE;

  g_option_context_set_summary (context, "Flatpak system helper");
  g_option_context_add_main_entries (context, options, GETTEXT_PACKAGE);

  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("%s: %s", g_get_application_name (), error->message);
      g_printerr ("\n");
      g_printerr ("Try \"%s --help\" for more information.",
                  g_get_prgname ());
      g_printerr ("\n");
      g_option_context_free (context);
      return 1;
    }

  if (show_version)
    {
      g_print (PACKAGE_STRING "\n");
      return 0;
    }

  if (verbose)
    g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG, message_handler, NULL);

  if (!on_session_bus)
    {
      authority = polkit_authority_get_sync (NULL, &error);
      if (authority == NULL)
        {
          g_printerr ("Can't get polkit authority: %s\n", error->message);
          return 1;
        }
    }

  exe_path_len = readlink ("/proc/self/exe", exe_path, sizeof (exe_path) - 1);
  if (exe_path_len > 0)
    {
      exe_path[exe_path_len] = 0;
      GFileMonitor *monitor;
      g_autoptr(GFile) exe = NULL;
      g_autoptr(GError) local_error = NULL;

      exe = g_file_new_for_path (exe_path);
      monitor =  g_file_monitor_file (exe,
                                      G_FILE_MONITOR_NONE,
                                      NULL,
                                      &local_error);
      if (monitor == NULL)
        g_warning ("Failed to set watch on %s: %s", exe_path, error->message);
      else
        g_signal_connect (monitor, "changed",
                          G_CALLBACK (binary_file_changed_cb), NULL);
    }

  flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
  if (replace)
    flags |= G_BUS_NAME_OWNER_FLAGS_REPLACE;

  name_owner_id = g_bus_own_name (on_session_bus ? G_BUS_TYPE_SESSION  : G_BUS_TYPE_SYSTEM,
                                  "org.freedesktop.Flatpak.SystemHelper",
                                  flags,
                                  on_bus_acquired,
                                  on_name_acquired,
                                  on_name_lost,
                                  NULL,
                                  NULL);

  ongoing_pulls = g_ptr_array_new ();

  /* Ensure we don't idle exit */
  schedule_idle_callback ();

  main_loop = g_main_loop_new (NULL, FALSE);
  g_main_loop_run (main_loop);

  g_clear_pointer (&ongoing_pulls, g_ptr_array_unref);

  return 0;
}
