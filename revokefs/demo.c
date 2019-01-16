#include <glib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>


static void
close_fd_before_exec (gpointer user_data)
{
  close (GPOINTER_TO_INT(user_data));
}

int
main (int argc, char *argv[])
{
  int sockets[2];
  pid_t pid;
  g_autofree char *socket_0 = NULL;
  g_autofree char *socket_1 = NULL;
  GError *error = NULL;
  char buf[20];
  GPid backend_pid, fuse_pid;

  if (argc != 3)
    {
      g_printerr ("Usage: revokefs-demo basepath targetpath\n");
      exit (EXIT_FAILURE);
    }

  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets))
    {
      perror ("Failed to create socket pair");
      exit (EXIT_FAILURE);
    }

  socket_0 = g_strdup_printf ("--socket=%d", sockets[0]);
  socket_1 = g_strdup_printf ("--socket=%d", sockets[1]);

  char *backend_argv[] =
    {
     "./revokefs-fuse",
     "--backend",
     socket_0,
     argv[1],
     NULL
    };

  if (!g_spawn_async (NULL,
                      backend_argv,
                      NULL,
                      G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                      /* Don't inherit fuse socket in backend */
                      close_fd_before_exec, GINT_TO_POINTER(sockets[1]),
                      &backend_pid, &error))
    {
      g_printerr ("Failed to launch backend: %s", error->message);
      exit (EXIT_FAILURE);
    }
  close (sockets[0]); /* Close backend side now so it doesn't get into the fuse child */

  char *fuse_argv[] =
    {
     "./revokefs-fuse",
     socket_1,
     argv[1],
     argv[2],
     NULL
    };

  if (!g_spawn_async (NULL,
                      fuse_argv,
                      NULL,
                      G_SPAWN_LEAVE_DESCRIPTORS_OPEN,
                      NULL, NULL,
                      &fuse_pid, &error))
    {
      g_printerr ("Failed to launch backend: %s", error->message);
      exit (EXIT_FAILURE);
    }

  g_print ("Started revokefs, press enter to revoke");
  fgets(buf, sizeof(buf), stdin);
  g_print ("Revoking write permissions");
  shutdown (sockets[1], SHUT_RDWR);
}
