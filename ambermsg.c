/* ambermsg: tiny command-line client for AmberWM's IPC socket.
 *
 * Usage:
 *   ambermsg status
 *   ambermsg workspace 3          ambermsg focus next|prev
 *   ambermsg close                ambermsg reload
 *   ambermsg dispatch view,2      (raw mango-style pass-through)
 *   ambermsg watch                (stream state pushes until Ctrl+C)
 *
 * Any other words are joined and sent verbatim, so new server-side
 * commands work without touching this file.
 */
#include <errno.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_ipc(void) {
	char path[512];
	path[0] = '\0';
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime == NULL) {
		runtime = "/tmp";
	}

	/* AmberWM's own socket always wins: the host session may export
	 * MANGO_INSTANCE_SIGNATURE for a DIFFERENT compositor (real
	 * mango), and we must not talk to that. */
	char pattern[512];
	snprintf(pattern, sizeof(pattern), "%s/amberwm-*.sock", runtime);
	glob_t gl;
	bool found = glob(pattern, GLOB_NOSORT, NULL, &gl) == 0;
	if (found && gl.gl_pathc >= 1) {
		snprintf(path, sizeof(path), "%s",
			gl.gl_pathv[gl.gl_pathc - 1]); // newest instance
	}
	size_t matches = found ? gl.gl_pathc : 0;
	globfree(&gl);
	if (matches > 1) {
		fprintf(stderr,
			"ambermsg: %zu amberwm instances, using %s "
			"(set MANGO_INSTANCE_SIGNATURE to disambiguate)\n",
			matches, path);
	}

	if (path[0] == '\0') {
		/* No local amberwm socket: honour mango-style discovery. */
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

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		return -1;
	}
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "ambermsg: cannot connect to %s: %s\n",
			path, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: ambermsg <command> [args...]\n"
			"  status | reload | close | quit\n"
			"  workspace N | focus next|prev\n"
			"  dispatch view,N | focusid client,ID\n"
			"  watch\n");
		return 1;
	}

	char req[1024];
	size_t len = 0;
	for (int i = 1; i < argc && len < sizeof(req) - 2; i++) {
		if (i > 1) {
			req[len++] = ' ';
		}
		len += (size_t)snprintf(req + len, sizeof(req) - len - 1,
			"%s", argv[i]);
	}
	req[len++] = '\n';
	req[len] = '\0';

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
