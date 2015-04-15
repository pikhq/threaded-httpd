#define _GNU_SOURCE
#include <limits.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <syslog.h>
#include <errno.h>
#include <poll.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <pwd.h>

struct request {
	char buf[4096];
	size_t buf_i;
	uint16_t url;
	uint16_t cur;
};

enum {
	HTTP_UNKNOWN,
	HTTP_GET,
	HTTP_HEAD,
	HTTP_OPTIONS,
	HTTP_PUT,
	HTTP_POST,
	HTTP_DELETE,
	HTTP_CONNECT,
	HTTP_TRACE
};

static sem_t sem;
static pthread_attr_t attr;

static ssize_t full_read(int fd, void *buf, size_t n)
{
	ssize_t len;
	ssize_t tot;
	while(n) {
		len = read(fd, buf, n);
		if(len < 0) return len;
		tot += len;
		buf += len;
		n -= len;
	}
}

static ssize_t full_write(int fd, const void *buf, size_t n)
{
	ssize_t len;
	ssize_t tot;
	while(n) {
		len = write(fd, buf, n);
		if(len < 0) return len;
		tot += len;
		buf += len;
		n -= len;
	}
}

static void http_error(int fd, int code)
{
	dprintf(fd, "HTTP/1.0 %d Error\r\n\r\n%d error\r\n", code, code);
}

static int read_request(int fd, struct request *req)
{
	memset(req->buf, 0, sizeof(req->buf));
	while(1) {
		ssize_t n;
		n = read(fd, req->buf + req->buf_i, sizeof(req->buf) - req->buf_i);
		if(n < 0) {
			syslog(LOG_ERR, "read: %m");
			return -1;
		}
		req->buf_i += n;
		if(memmem(req->buf, req->buf_i, "\r\n\r\n", 4)) return 0;
		if(req->buf_i == sizeof(req->buf)) {
			if(memmem(req->buf, req->buf_i, "\r\n", 2))
				http_error(fd, 414);
			else
				http_error(fd, 413);
			return -1;
		}
		if(n == 0) {
			http_error(fd, 400);
			return -1;
		}
	}
}

static int get_method(struct request *req)
{
	if(!strncmp("GET ", req->buf, 4))
		return req->cur += 4, HTTP_GET;
	if(!strncmp("HEAD ", req->buf, 5))
		return req->cur += 5, HTTP_HEAD;
	if(!strncmp("OPTIONS ", req->buf, 8))
		return req->cur += 8, HTTP_OPTIONS;
	if(!strncmp("PUT ", req->buf, 4))
		return req->cur += 4, HTTP_PUT;
	if(!strncmp("POST ", req->buf, 5))
		return req->cur += 5, HTTP_POST;
	if(!strncmp("DELETE ", req->buf, 7))
		return req->cur += 7, HTTP_DELETE;
	if(!strncmp("CONNECT ", req->buf, 8))
		return req->cur += 8, HTTP_CONNECT;
	if(!strncmp("TRACE ", req->buf, 6))
		return req->cur += 6, HTTP_TRACE;
	return HTTP_UNKNOWN;
}

static int fromhex(char c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

static void do_dir_list(int c, int f)
{
	/*
	struct dirent **namelist;
	int count;
	count = scandirat(f, ".", &namelist, filter, alphasort);
	if(count < 0) {
		http_error(c, 500);
		return;
	}
	dprintf(2, "HTTP/1.0 200 OK\r\n"
	       "Content-Type: text/html; charset=UTF-8\r\n"
	       "\r\n"
	       "<!DOCTYPE html>\r\n");
	for(int i = 0; i < count; i++) {
		dprintf(2, "<a href=\"/");
		url_enc(s);
		dprintf(2, "/");
		url_enc(namelist[i]->d_name);
		dprintf(2, "\">");
		for(size_t j = 0; namelist[i]->d_name[j]; j++) {
			switch(namelist[i]->d_name[j]) {
			case '<':
				dprintf(2, "&lt;");
				break;
			case '>':
				dprintf(2, "&gt;");
				break;
			case '&':
				dprintf(2, "&amp;");
				break;
			case '"':
				dprintf(2, "&quot;");
				break;
			case '\'':
				dprintf(2, "&apos;");
				break;
			default:
				dprintf(2, "%c", namelist[i]->d_name[j]);
			}
		}
		dprintf(2, "</a><br/>\r\n");
	}
	free(namelist);
	*/
	http_error(c, 503);
}

static char *get_path(struct request *req, char **host)
{
	char *ret, *in, *out;
	if(!strncmp("http://", req->buf + req->cur, 7)) {
		char *slash;
		req->cur += 7;
		*host = req->buf + req->cur;
		slash = strpbrk(req->buf + req->cur, "/ \r\n");
		if(!slash || *slash != '/') return 0;
		req->cur += slash - (req->buf + req->cur);
	}

	ret = req->buf + req->cur;

	for(in = out = ret; *in != ' ' && *in != '\r' && *in != '\n' ; in++, out++) {
		if(*in == '%') {
			int n;
			char c = 0;
			if(!*++in) return 0;
			if(!in[1]) return 0;
			n = fromhex(*in);
			if(n < 0) return 0;
			c = n << 4;
			n = fromhex(*++in);
			if(n < 0) return 0;
			c |= n;
			if(c == '/') return 0;
			*out = c;
		} else {
			*out = *in;
		}
	}

	if(*in == '\r' || *in == '\n') return 0;
	*out = '\0';
	return ret;
}

void *thread(void *fd_p)
{
	int fd = *(int*)fd_p;

	while(1) {
		int c, f;
		struct request req = {0};
		struct stat buf;
		char *url, *tmp;
		ssize_t n;

		sem_post(&sem);
		c = accept(fd, (struct sockaddr[]){0}, (socklen_t[]){sizeof(struct sockaddr)});
		if(c == -1) {
			if(errno == EAGAIN || errno == EWOULDBLOCK) {
				if(sem_trywait(&sem) == -1) {
					poll((struct pollfd[]){ {fd, POLLIN, 0} }, 1, 0);
					continue;
				}
				return 0;
			} else {
				syslog(LOG_ERR, "accept: %m");
				sem_trywait(&sem);
				continue;
			}
		}
		sem_trywait(&sem);
		
		if(sem_trywait(&sem) == -1) {
			// sem_post(&sem);
			int val;
			if((val = pthread_create(&(pthread_t){0}, &attr, thread, fd_p)) != 0) {
				errno = val;
				syslog(LOG_ERR, "pthread_create: %m");
			}
		} else {
			sem_post(&sem);
		}


		if(read_request(c, &req) < 0) {
			close(c);
			continue;
		}
		switch(get_method(&req)) {
		case HTTP_GET:
			break;
		default:
			http_error(c, 501);
			close(c);
			continue;
		}

		url = get_path(&req, &(char*){0});
		if(!url) {
			http_error(c, 400);
			close(c);
			continue;
		}

		if((tmp = strstr(url, "/../"))) {
			http_error(c, 403);
			close(c);
			continue;
		}
		if((tmp = strrchr(url, '/'))) {
			if(strcmp(tmp, "/..") == 0) {
				http_error(c, 403);
				close(c);
				continue;
			}
		}
		for(tmp = url; *tmp == '/'; tmp++);
		if(strcmp(tmp, "") == 0) tmp = ".";
		f = open(tmp, O_RDONLY);
		if(f < 0) {
			switch(errno) {
			case ENOENT:
				http_error(c, 404);
				break;
			case EACCES:
				http_error(c, 403);
				break;
			case ENFILE:
				http_error(c, 503);
				break;
			default:
				http_error(c, 500);
				break;
			}
			close(c);
			continue;
		}

		if(fstat(f, &buf) < 0) {
			http_error(c, 500);
			close(c);
			close(f);
			continue;
		}

		if(S_ISDIR(buf.st_mode)) {
			int tmp_f;
			tmp_f = openat(f, "index.html", O_RDONLY);
			if(tmp_f >= 0) {
				close(f);
				f = tmp_f;
			} else {
				do_dir_list(c, f);
			}
			close(c);
			close(f);
			continue;
		}

		dprintf(c, "HTTP/1.0 200 OK\r\n"
			   "Content-Length: %jd\r\n\r\n", (intmax_t)buf.st_size);

		while((n = sendfile(c, f, 0, 500 * 1024 * 1024)) > 0);
		close(c);
		close(f);
	}
}


static int fd;

static void handle(int n)
{
	shutdown(fd, SHUT_RDWR);
	close(fd);
	_Exit(50);
}

int main()
{
	pthread_attr_init(&attr);
	sem_init(&sem, 0, 0);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN > 12288 ? PTHREAD_STACK_MIN : 12288);

	int status;
	struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_PASSIVE
	};
	struct addrinfo *res;

	signal(SIGPIPE, SIG_IGN);

	if((status = getaddrinfo(NULL, "8080", &hints, &res)) != 0) {
		fprintf(stderr, "error: %s\n", gai_strerror(status));
		return 1;
	}

	prctl(PR_SET_NO_NEW_PRIVS, 1);
	if(!chroot(".")) {
		chdir("/");
	}
	struct passwd *pw;
	pw = getpwnam("nobody");
	if(pw) {
		setuid(pw->pw_uid);
	}

	fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if(fd < 0) {
		perror("error");
		return 1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (int[]){1}, sizeof(int));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &(struct timeval){.tv_sec = 30}, sizeof(struct timeval));
	if(bind(fd, res->ai_addr, res->ai_addrlen) < 0) {
		perror("error");
		return 1;
	}

	if(listen(fd, SOMAXCONN) < 0) {
		perror("error");
		return 1;
	}


	signal(SIGHUP, handle);
	signal(SIGINT, handle);
	signal(SIGALRM, handle);
	signal(SIGTERM, handle);

	for(;;)
		thread(&fd);
}
