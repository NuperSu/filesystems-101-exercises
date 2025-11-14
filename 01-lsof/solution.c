#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static char *readlink_to_string(const char *path) {
	size_t cap = 256;
	char *buf = (char *)malloc(cap);
	if (!buf) return NULL;
	while (1) {
		ssize_t n = readlink(path, buf, cap - 1);
		if (n < 0) {
			report_error(path, errno);
			free(buf);
			return NULL;
		}
		if ((size_t)n < cap - 1) {
			buf[n] = '\0';
			return buf;
		}
		if (cap > SIZE_MAX / 2) { 
			free(buf);
			return NULL;
		}
		cap *= 2;
		char *nbuf = (char *)realloc(buf, cap);
		if (!nbuf) { free(buf); return NULL; }
		buf = nbuf;
	}
}

void lsof(void)
{
	DIR *proc = opendir("/proc");
	if (!proc) {
		report_error("/proc", errno);
		return;
	}

	struct dirent *entry;
	while ((entry = readdir(proc)) != NULL) {
		if (!is_number(entry->d_name)) continue;
		pid_t pid = (pid_t)strtol(entry->d_name, NULL, 10);

		char *fd_dir_path = construct_path(pid, "fd");
		if (!fd_dir_path) continue;

		DIR *fd_dir = opendir(fd_dir_path);
		if (!fd_dir) {
			report_error(fd_dir_path, errno);
			free(fd_dir_path);
			continue;
		}

		struct dirent *file_entry;
		while ((file_entry = readdir(fd_dir)) != NULL) {
			if (!is_number(file_entry->d_name)) continue;

			size_t need = strlen(fd_dir_path) + 1 + strlen(file_entry->d_name) + 1;
			char *fd_path = (char *)malloc(need);
			if (!fd_path) continue;
			
			int n = snprintf(fd_path, need, "%s/%s", fd_dir_path, file_entry->d_name);
			if (n < 0 || (size_t)n >= need) {
				free(fd_path);
				continue;
			}
			
			char *target = readlink_to_string(fd_path);
			if (target) {
				report_file(target);
				free(target);
			}
			free(fd_path);
		}

		closedir(fd_dir);
		free(fd_dir_path);
	}

	closedir(proc);
}
