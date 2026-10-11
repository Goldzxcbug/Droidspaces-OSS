/*
 * Droidspaces - anland display daemon integration
 *
 * A container with anland enabled gets its own broker daemon: a forked process
 * that runs the vendored display daemon's epoll loop on a per-container socket
 * under the workspace. The Android consumer app reaches that socket through
 * its root helper, and the patched compositor inside the container reaches it
 * at /run/display.sock, where the host socket is bind-mounted after pivot. The
 * daemon brokers their handshake and nothing else.
 *
 * Modeled on src/android/x11.c. Unlike the X server, which is one global
 * process, every container has its own daemon, so the pid file and the socket
 * record are keyed by container name.
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspace.h"

#include "display_daemon.h"

/* Per-container record filenames in the Pids dir. Their extensions are
 * deliberately not DS_EXT_PID: every Pids scanner treats <x>.pid as a container
 * named x, so a daemon pid file named <name>.anland.pid would show up as a
 * phantom container and be pruned as such when the daemon dies. */
#define ANLAND_PID_EXT ".apid"
#define ANLAND_SOCK_EXT ".asock"

static void anland_record_name(const struct ds_config *cfg, const char *ext,
                               char *buf, size_t n) {
  char safe[NAME_MAX + 1];
  sanitize_container_name(cfg->container_name, safe, sizeof(safe));
  snprintf(buf, n, "%s%s", safe, ext);
}

/* anland is per-container, so a daemon is never shared between containers:
 * ds_global_daemon_stop's "keep alive for others" check is always false. */
static int anland_never_needed(void) { return 0; }

/* The vendored daemon installs no signal handlers; the host process owns
 * shutdown. SIGTERM from ds_global_daemon_stop has to reach the loop flag, or
 * daemon_destroy never runs and the socket is left behind. */
static daemon_ctx *g_anland_ctx;
static void anland_on_term(int sig) {
  (void)sig;
  daemon_stop(g_anland_ctx);
}

/*
 * The forked daemon process. The vendored daemon is linked into this binary,
 * so there is no exec: close every inherited descriptor above stderr, bind the
 * socket, report readiness, then run the loop until SIGTERM. ready_fd is the
 * write end of ds_spawn_daemon's pipe: closed on success, written on failure.
 */
static void anland_child(int ready_fd, void *user_data) {
  const char *sock_path = user_data;

  ds_daemon_child_preamble();
  setsid();
  signal(SIGTERM, anland_on_term);

  /* Nothing the launcher had open belongs to the daemon: the console PTY pair,
   * image and netlink descriptors, the request pipes in daemon mode. Without an
   * exec nothing closes them for us. */
  int keep = ready_fd;
  for (int fd = 3; fd < 1024; fd++)
    if (fd != keep)
      close(fd);
  int devnull = open("/dev/null", O_RDONLY);
  if (devnull >= 0) {
    dup2(devnull, STDIN_FILENO);
    close(devnull);
  }

  if (daemon_create(&g_anland_ctx, sock_path) < 0) {
    if (write(ready_fd, "\x01", 1) < 0) { /* ignore */
    }
    _exit(1);
  }
  close(ready_fd);
  daemon_run(g_anland_ctx);
  daemon_destroy(g_anland_ctx);
  _exit(0);
}

/* Load the recorded socket path (Pids/<name>.asock) into cfg->anland_sock.
 * The path is runtime state, not part of container.config, so a cfg loaded
 * from disk at stop or show time has it empty. */
void ds_anland_load_sock(struct ds_config *cfg) {
  char name[NAME_MAX + 16], rec[PATH_MAX];
  anland_record_name(cfg, ANLAND_SOCK_EXT, name, sizeof(name));
  snprintf(rec, sizeof(rec), "%s/%s", get_pids_dir(), name);
  if (read_file(rec, cfg->anland_sock, sizeof(cfg->anland_sock)) < 0)
    cfg->anland_sock[0] = '\0';
  cfg->anland_sock[strcspn(cfg->anland_sock, "\r\n")] = '\0';
}

int ds_anland_daemon_start(struct ds_config *cfg) {
  if (!cfg || !cfg->anland || !is_android())
    return -1;
  if (getuid() != 0) {
    ds_error("[Anland] not running as root");
    return -1;
  }

  char pidfile[NAME_MAX + 16], sockfile[NAME_MAX + 16];
  anland_record_name(cfg, ANLAND_PID_EXT, pidfile, sizeof(pidfile));
  anland_record_name(cfg, ANLAND_SOCK_EXT, sockfile, sizeof(sockfile));

  /* A restart from inside the container keeps the daemon; reuse it. */
  pid_t existing = ds_daemon_read_pid(pidfile);
  if (existing > 0) {
    cfg->anland_pid = existing;
    ds_anland_load_sock(cfg);
    ds_log("[Anland] daemon already running (PID %d)", (int)existing);
    return 1;
  }

  /* The socket lives in the workspace's Net dir, which only root can write, so
   * nobody can swap it for a symlink between bind, chmod and the bind mount.
   * The name is random so the path is not guessable from outside. */
  char uuid[DS_UUID_LEN + 1];
  if (generate_uuid(uuid, sizeof(uuid)) < 0) {
    ds_error("[Anland] failed to generate socket name");
    return -1;
  }
  snprintf(cfg->anland_sock, sizeof(cfg->anland_sock), "%s/anland-%s.sock",
           get_net_dir(), uuid);
  ds_log("[Anland] launching display daemon on %s", cfg->anland_sock);

  /* The log relay resolves its file relative to get_logs_dir(); the container's
   * own log directory is already there from ds_open_container_log. */
  char safe[NAME_MAX + 1], log_rel[NAME_MAX + 16];
  sanitize_container_name(cfg->container_name, safe, sizeof(safe));
  snprintf(log_rel, sizeof(log_rel), "%s/anland", safe);

  pid_t child = ds_spawn_daemon(anland_child, cfg->anland_sock, log_rel,
                                "anland", "Anland");
  if (child <= 0) {
    cfg->anland_sock[0] = '\0';
    return -1;
  }

  /* The compositor inside the container may run as a normal user, and connect
   * needs write permission on the socket inode. */
  chmod(cfg->anland_sock, 0666);

  cfg->anland_pid = child;
  ds_daemon_write_pid(pidfile, child);
  char rec[PATH_MAX];
  snprintf(rec, sizeof(rec), "%s/%s", get_pids_dir(), sockfile);
  write_file_atomic(rec, cfg->anland_sock);
  return 0;
}

void ds_anland_daemon_stop(struct ds_config *cfg) {
  if (!cfg)
    return;
  char pidfile[NAME_MAX + 16], sockfile[NAME_MAX + 16];
  anland_record_name(cfg, ANLAND_PID_EXT, pidfile, sizeof(pidfile));
  anland_record_name(cfg, ANLAND_SOCK_EXT, sockfile, sizeof(sockfile));

  if (cfg->anland_sock[0] == '\0')
    ds_anland_load_sock(cfg);

  ds_global_daemon_stop(anland_never_needed, cfg->anland_pid, &cfg->anland_pid,
                        pidfile, cfg->anland_sock[0] ? cfg->anland_sock : NULL,
                        "[Anland]");
  ds_daemon_remove_pid(sockfile);
}

/* Post-pivot: bind-mount the host socket to /run/display.sock, where the
 * patched compositor expects it. */
int ds_setup_anland_socket(struct ds_config *cfg) {
  if (!cfg || !cfg->anland || !is_android())
    return 0;
  if (cfg->anland_sock[0] == '\0') {
    ds_warn("[Anland] no daemon socket recorded - skipping bind mount");
    return 0;
  }

  char src[PATH_MAX + 32];
  snprintf(src, sizeof(src), "/.old_root%s", cfg->anland_sock);
  if (access(src, F_OK) != 0) {
    ds_warn("[Anland] host socket not found at %s - skipping bind mount", src);
    return 0;
  }
  if (ds_bind_mount_socket(src, "/run/display.sock", 0, "anland") < 0)
    return 0;

  ds_log("[Anland] display socket bind-mounted to /run/display.sock");
  return 0;
}
