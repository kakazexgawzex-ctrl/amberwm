/* ambermsg: command-line client for AmberWM's IPC socket.
 *
 * Usage:
 *   ambermsg status                      compositor state JSON
 *   ambermsg clients                     list open windows
 *   ambermsg version                     print version
 *   ambermsg workspace N                 switch to workspace N
 *   ambermsg focus next|prev             cycle focus
 *   ambermsg focus ID                    focus window by numeric id
 *   ambermsg close [ID]                  close window (focused if no id)
 *   ambermsg enable FEATURE              animations | blur | shadows |
 *   ambermsg disable FEATURE             center-focused-column | ws-slide
 *   ambermsg reload | quit
 *   ambermsg watch                       stream state pushes until Ctrl+C
 *   ambermsg call ARGS...                raw protocol pass-through
 *
 * Any other words are joined and sent verbatim, so new server-side
 * commands work without touching this file.
 */
#include <errno.h>
#include <glob.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int ipc_dial(const char *path, bool verbose) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (verbose) {
			fprintf(stderr, "ambermsg: cannot connect to %s: %s\n",
				path, strerror(errno));
		}
		close(fd);
		return -1;
	}
	return fd;
}

static int connect_ipc(void) {
	char path[512];
	path[0] = '\0';
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL) {
		runtime = "/tmp";
	}

	/* An explicit AMBERWM_IPC_SOCKET always wins over scanning: stale
	 * socket files from crashed sessions would otherwise be picked. */
	const char *own = getenv("AMBERWM_IPC_SOCKET");
	if (own != NULL && own[0] != '\0') {
		snprintf(path, sizeof(path), "%s", own);
	}

	/* Probe candidates for a LIVE listener: crashed sessions leave
	 * socket files behind that accept nothing. */
	char fallback[512];
	fallback[0] = '\0';
	if (path[0] == '\0') {
		char pattern[512];
		snprintf(pattern, sizeof(pattern), "%s/amberwm-*.sock",
			runtime);
		glob_t gl;
		if (glob(pattern, GLOB_NOSORT, NULL, &gl) == 0) {
			for (size_t i = 0; i < gl.gl_pathc; i++) {
				snprintf(fallback, sizeof(fallback), "%s",
					gl.gl_pathv[i]);
				int pfd = ipc_dial(fallback, false);
				if (pfd >= 0) {
					close(pfd);
					snprintf(path, sizeof(path), "%s",
						fallback);
					break;
				}
			}
			globfree(&gl);
		}
	}
	if (path[0] == '\0' && fallback[0] != '\0') {
		/* Nothing answered; report against the last candidate. */
		snprintf(path, sizeof(path), "%s", fallback);
	}

	if (path[0] == '\0') {
		/* Legacy sessions still export the mango-style variable. */
		const char *sig = getenv("MANGO_INSTANCE_SIGNATURE");
		if (sig != NULL && sig[0] != '\0') {
			if (sig[0] != '/') {
				snprintf(path, sizeof(path), "%s/%s",
					runtime, sig);
			} else {
				snprintf(path, sizeof(path), "%s", sig);
			}
		}
	}

	if (path[0] == '\0') {
		const char *display = getenv("WAYLAND_DISPLAY");
		if (display == NULL) {
			display = "wayland-0";
		}
		snprintf(path, sizeof(path), "%s/amberwm-%s.sock", runtime,
			display);
	}

	return ipc_dial(path, true);
}

static void usage(FILE *out) {
	fprintf(out,
		"usage: ambermsg <command> [args...]\n"
		"\n"
		"  status                    compositor state as JSON\n"
		"  clients                   list open windows\n"
		"  version                   print compositor version\n"
		"  workspace N               switch to workspace N (1-9)\n"
		"  focus next|prev           cycle focus\n"
		"  focus ID                  focus window by id\n"
		"  close [ID]                close window (focused if no id)\n"
		"\n"
		"  enable FEATURE            toggle at runtime:\n"
		"  disable FEATURE           animations blur shadows\n"
		"                            center-focused-column ws-slide\n"
		"\n"
		"  reload                    hot-reload config\n"
		"  quit                      exit the compositor\n"
		"  watch                     stream state until Ctrl+C\n"
		"  call ARGS...              raw protocol pass-through\n");
}

int main(int argc, char **argv) {
	if (argc < 2 || strcmp(argv[1], "help") == 0 ||
			strcmp(argv[1], "--help") == 0) {
		usage(argc < 2 ? stderr : stdout);
		return argc < 2 ? 1 : 0;
	}

	char req[1024];
	size_t len = 0;

	/* Friendly front-end: translate to wire commands. */
	const char *cmd = argv[1];
	if (strcmp(cmd, "clients") == 0) {
		len = (size_t)snprintf(req, sizeof(req), "get all-clients\n");
	} else if (strcmp(cmd, "version") == 0) {
		len = (size_t)snprintf(req, sizeof(req), "get version\n");
	} else if (strcmp(cmd, "focus") == 0 && argc >= 3 &&
			strcmp(argv[2], "next") != 0 &&
			strcmp(argv[2], "prev") != 0) {
		len = (size_t)snprintf(req, sizeof(req),
			"dispatch focusid client,%s\n", argv[2]);
	} else if (strcmp(cmd, "call") == 0) {
		for (int i = 2; i < argc && len < sizeof(req) - 2; i++) {
			if (i > 2) {
				req[len++] = ' ';
			}
			len += (size_t)snprintf(req + len,
				sizeof(req) - len - 1, "%s", argv[i]);
		}
		req[len++] = '\n';
		req[len] = '\0';
	} else if (strcmp(cmd, "enable") == 0 || strcmp(cmd, "disable") == 0) {
		for (int i = 1; i < argc && len < sizeof(req) - 2; i++) {
			if (i > 1) {
				req[len++] = ' ';
			}
			len += (size_t)snprintf(req + len,
				sizeof(req) - len - 1, "%s", argv[i]);
		}
		req[len++] = '\n';
		req[len] = '\0';
	} else {
		/* Everything else maps 1:1 onto the wire protocol. */
		for (int i = 1; i < argc && len < sizeof(req) - 2; i++) {
			if (i > 1) {
				req[len++] = ' ';
			}
			len += (size_t)snprintf(req + len,
				sizeof(req) - len - 1, "%s", argv[i]);
		}
		req[len++] = '\n';
		req[len] = '\0';
	}

	int fd = connect_ipc();
	if (fd < 0) {
		return 1;
	}
	if (write(fd, req, len) < 0) {
		perror("ambermsg: write");
		close(fd);
		return 1;
	}

	char buf[4096];
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		if (n > 0) {
			fwrite(buf, 1, (size_t)n, stdout);
			fflush(stdout);
			continue;
		}
		break; // EOF or error: server closes after answering
	}
	close(fd);
	return 0;
}
