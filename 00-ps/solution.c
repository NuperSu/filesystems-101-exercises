#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <solution.h>

static int is_number(const char *s) {
	if (*s == '\0') return 0;
	for (const char *p = s; *p; ++p) {
		if (*p < '0' || *p > '9') return 0;
	}
	return 1;
}


static char *construct_path(pid_t pid, const char *suffix) {
	char buf[64];
	int n = snprintf(buf, sizeof(buf), "/proc/%ld/", (long)pid);
	if (n < 0 || (size_t)n >= sizeof(buf)) return NULL;
	size_t need = (size_t)n + strlen(suffix) + 1;
	char *out = (char *)malloc(need);
	if (!out) return NULL;
	memcpy(out, buf, (size_t)n);
	memcpy(out + n, suffix, strlen(suffix) + 1);
	return out;
}

static char *read_exe_path(pid_t pid) {
	char *path = construct_path(pid, "exe");
	if (!path) return NULL;
        // cap is dynamicly increased
	size_t cap = 256;
	char *buf = (char *)malloc(cap);
	if (!buf) { 
			free(path);
			return NULL;
	}
	while (1) {
		ssize_t n = readlink(path, buf, cap - 1);
		if (n < 0) {
			report_error(path, errno);
			free(path);
			free(buf);
			return NULL;
		}
		if ((size_t)n < cap - 1) {
			buf[n] = '\0';
			break;
		}
		if (cap > SIZE_MAX / 2) {
			free(path);
			free(buf);
			return NULL;
		}
		cap *= 2;
		char *nbuf = (char *)realloc(buf, cap);
		if (!nbuf) { free(path); free(buf); return NULL; }
		buf = nbuf;
	}
	free(path);
	return buf;
}

static char **read_nul_sep_file(pid_t pid, const char *name, size_t *out_count) {
	char *path = construct_path(pid, name);
	if (!path) return NULL;
	FILE *f = fopen(path, "rb");
	if (!f) {
		report_error(path, errno);
		free(path);
		char **empty = (char **)calloc(1, sizeof(char *));
		if (out_count) *out_count = 0;
		return empty;
	}
        // cap is dynamicly increased
	size_t cap = 4096, len = 0;
	unsigned char *buf = (unsigned char *)malloc(cap);
	if (!buf) { fclose(f); free(path); return NULL; }
	size_t nread;
	while ((nread = fread(buf + len, 1, cap - len, f)) > 0) {
		len += nread;
		if (len == cap) {
			cap *= 2;
			unsigned char *nbuf = (unsigned char *)realloc(buf, cap);
			if (!nbuf) { free(buf); fclose(f); free(path); return NULL; }
			buf = nbuf;
		}
	}
	fclose(f);
	free(path);

	if (len == 0 || buf[len - 1] != '\0') {
		unsigned char *nbuf = (unsigned char *)realloc(buf, len + 1);
		if (!nbuf) { free(buf); return NULL; }
		buf = nbuf;
		buf[len++] = '\0';
	}

	size_t count = 0;
	for (size_t i = 0; i < len; ) {
		size_t start = i;
		while (i < len && buf[i] != '\0') i++;
		if (i > start) count++;
		i++;
	}

	char **vec = (char **)malloc((count + 1) * sizeof(char *));
	if (!vec) { free(buf); return NULL; }
	size_t idx = 0;
	for (size_t i = 0; i < len; ) {
		size_t start = i;
		while (i < len && buf[i] != '\0') i++;
		if (i > start) {
			size_t slen = i - start;
			char *s = (char *)malloc(slen + 1);
			if (s) {
				memcpy(s, buf + start, slen);
				s[slen] = '\0';
				vec[idx++] = s;
			}
		}
		i++;
	}
	vec[idx] = NULL;
	if (out_count) *out_count = idx;
	free(buf);
	return vec;
}

static void free_vec(char **vec) {
	if (!vec) return;
	for (char **p = vec; *p != NULL; ++p) free(*p);
	free(vec);
}

void ps(void) {
	DIR *proc_dir = opendir("/proc");
	if (!proc_dir) {
		report_error("/proc", errno);
		return;
	}

	struct dirent *entry;
	while ((entry = readdir(proc_dir)) != NULL) {
		// Check if entry is a numeric directory (PID) 
		if (!is_number(entry->d_name)) continue;

		pid_t pid = (pid_t)strtol(entry->d_name, NULL, 10);

		char *exe_target = read_exe_path(pid);
		if (!exe_target) {
			char *t = (char*)malloc(1);
			// Reporting errors is only for accessing files
			if (!t) continue;
			t[0] = '\0';
			exe_target = t;
		}

		size_t argc = 0, envc = 0;
		char **argv = read_nul_sep_file(pid, "cmdline", &argc);
		if (!argv) argv = (char **)calloc(1, sizeof(char *));
		char **envp = read_nul_sep_file(pid, "environ", &envc);
		if (!envp) envp = (char **)calloc(1, sizeof(char *));
                // Constructing argv if it is not provided
		if (argc == 0 && exe_target && exe_target[0] != '\0') {
			char **argv2 = (char **)malloc(2 * sizeof(char *));
			if (argv2) {
				size_t nbytes = strlen(exe_target);
				char *dup = (char*)malloc(nbytes + 1);
				if (dup) { memcpy(dup, exe_target, nbytes + 1); }
				argv2[0] = dup;
				argv2[1] = NULL;
				free_vec(argv);
				argv = argv2;
			}
		}

		// Report the process
		report_process(pid, exe_target, argv, envp);

		// Clean up 
		free(exe_target);
		free_vec(argv);
		free_vec(envp);
	}

	closedir(proc_dir);
}
