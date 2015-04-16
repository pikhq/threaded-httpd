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
#include <ctype.h>
#include <netinet/tcp.h>

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

static void http_error(int fd, int code, char *http)
{
	static const struct {
		int c;
		char msg[25];
	} *msg, msgs[] = {
		{400, "Bad Request"},
		{403, "Forbidden"},
		{404, "Not Found"},
		{413, "Request Entity Too Large"},
		{414, "Request-URI Too Long"},
		{500, "Internal Server Error"},
		{501, "Not Implemented"},
		{503, "Service Unavailable"},
		{}
	};

	for(msg = msgs; msg->c && msg->c != code; msg++);

	if(strcmp(http, "HTTP/0.9") != 0)
		dprintf(fd, "HTTP/1.0 %d %s\r\n"
			    "Content-type: text/html; charset=UTF-8\r\n"
			    "\r\n",
			    code, msg->c ? msg->msg : "Error");
	dprintf(fd, "<!DOCTYPE html>\n"
		    "<title>%d %s</title>\n"
		    "<h1>%d Error</h1>\n"
		    "%s\n",
		    code, msg->c ? msg->msg : "error",
		    code,
		    msg->c ? msg->msg : "An error occured in the request.");
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
		if(req->buf_i >= 6) {
			// Search for an HTTP/0.9 request
			if(strncmp("GET ", req->buf, 4) == 0) {
				int i;
				for(i = 5; i < req->buf_i; i++) {
					if(req->buf[i] == ' '
					  || req->buf[i] == '\r'
					  || req->buf[i] == '\n')
						break;
				}
				if(req->buf[i] == '\r' || req->buf[i] == '\n')
					return 0;
			}
		}
		if(req->buf_i == sizeof(req->buf)) {
			if(memmem(req->buf, req->buf_i, "\r\n", 2))
				http_error(fd, 414, "HTTP/1.0");
			else
				http_error(fd, 413, "HTTP/1.0");
			return -1;
		}
		if(n == 0) {
			http_error(fd, 400, "HTTP/1.0");
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

static void url_enc(char *s, int f)
{
	for(; *s; s++) {
		char *c = (char[4]){ *s, 0 };
		if(!isalnum(*s) && *s != '-' && *s != '_' && *s != '.' && *s != '~')
			sprintf(c, "%%%.2X", (unsigned)*s);
		dprintf(f, "%s", c);
	}
}

static void do_dir_list(char *s, int c, int f, char *http)
{
	DIR *d = fdopendir(f);
	struct dirent *de, **names=0, **tmp;
	size_t cnt=0, len=0;

	if(!d) {
		http_error(c, 503, http);
		close(f);
		return;
	}

	while((errno=0), (de = readdir(d))) {
		if(de->d_name[0] == '.') continue;
		if(cnt >= len) {
			len = 2*len+1;
			if (len > SIZE_MAX/sizeof *names) break;
			tmp = realloc(names, len * sizeof *names);
			if (!tmp) break;
			names = tmp;
		}
		names[cnt] = malloc(de->d_reclen);
		if (!names[cnt]) break;
		memcpy(names[cnt++], de, de->d_reclen);
	}

	if(errno) {
		closedir(d);
		http_error(c, 503, http);
		if (names) while(cnt-->0) free(names[cnt]);
		free(names);
	}
	qsort(names, cnt, sizeof *names, (int (*)(const void *, const void *))alphasort);

	if(strcmp(http, "HTTP/0.9") != 0)
		dprintf(c, "HTTP/1.0 200 OK\r\n"
		       "Content-Type: text/html; charset=UTF-8\r\n"
		       "\r\n");
	dprintf(c, "<!DOCTYPE html>\r\n");
	for(size_t i = 0; i < cnt; i++) {
		dprintf(c, "<a href=\"/");
		url_enc(s, c);
		dprintf(c, "/");
		url_enc(names[i]->d_name, c);
		dprintf(c, "\">");
		for(size_t j = 0; names[i]->d_name[j]; j++) {
			switch(names[i]->d_name[j]) {
			case '<':
				dprintf(c, "&lt;");
				break;
			case '>':
				dprintf(c, "&gt;");
				break;
			case '&':
				dprintf(c, "&amp;");
				break;
			case '"':
				dprintf(c, "&quot;");
				break;
			case '\'':
				dprintf(c, "&apos;");
				break;
			default:
				dprintf(c, "%c", names[i]->d_name[j]);
			}
		}
		dprintf(c, "</a><br/>\r\n");
		free(names[i]);
	}

	free(names);
	closedir(d);
}

static char *get_path(struct request *req, char **host, char **http)
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

	*http = in;
	for(; *in != '\r' && *in != '\n'; in++);
	*in = '\0';
	if(strcmp(*http, "") == 0) *http = "HTTP/0.9";

	*out = '\0';
	return ret;
}

static const char *mime_type(char *path)
{
	char *end = strrchr(path, '.');
	static const struct {
		char *ext;
		char *mime;
	} *p, map[] = {
		{ ".pdf", "application/pdf" },
		{ ".ogg", "application/ogg" },
		{ ".bin", "application/octet-stream" },
		{ ".json", "application/json; charset=UTF-8" },
		{ ".ps", "application/postscript" },
		{ ".rdf", "application/rdf+xml; charset=UTF-8" },
		{ ".rss", "application/rss+xml; charset=UTF-8" },
		{ ".xml", "application/xml; charset=UTF-8" },
		{ ".xhtml", "application/xhtml+xml; charset=UTF-8" },
		{ ".zip", "application/zip" },
		{ ".gz", "application/gzip" },
		{ ".tar", "application/x-tar" },
		{ ".atom", "application/atom+xml; charset=UTF-8" },
		{ ".woff", "application/font-woff" },
		{ ".m4a", "audio/mp4" },
		{ ".mp1", "audio/mpeg" },
		{ ".mp2", "audio/mpeg" },
		{ ".mp3", "audio/mpeg" },
		{ ".m1a", "audio/mpeg" },
		{ ".m2a", "audio/mpeg" },
		{ ".mpa", "audio/mpeg" },
		{ ".oga", "audio/ogg" },
		{ ".flac", "audio/flac" },
		{ ".opus", "audio/opus" },
		{ ".ra", "audio/vnd.rn-realaudio" },
		{ ".wav", "audio/vnd.wave" },
		{ ".webm", "video/webm" },
		{ ".gif", "image/gif" },
		{ ".jpeg", "image/jpeg" },
		{ ".jpg", "image/jpeg" },
		{ ".png", "image/png" },
		{ ".svg", "image/svg+xml; charset=UTF-8" },
		{ ".tiff", "image/tiff" },
		{ ".djvu", "image/vnd.djvu" },
		{ ".djv", "image/vnd.djvu" },
		{ ".txt", "text/plain; charset=UTF-8" },
		{ ".vcf", "text/vcard; charset=UTF-8" },
		{ ".vcard", "text/vcard; charset=UTF-8" },
		{ ".html", "text/html; charset=UTF-8" },
		{ ".htm", "text/html; charset=UTF-8" },
		{ ".css", "text/css; charset=UTF-8" },
		{ ".avi", "video/avi" },
		{ ".mpg", "video/mpeg" },
		{ ".mpeg", "video/mpeg" },
		{ ".m1v", "video/mpeg" },
		{ ".mpv", "video/mpeg" },
		{ ".mp4", "video/mp4" },
		{ ".m4v", "video/mp4" },
		{ ".mkv", "video/x-matroska" },
		{ ".wmv", "video/x-ms-wmv" },
		{ ".flv", "video/x-flv" },
		{ }
	};
	if(!end) return NULL;
	for(p = map; p->ext; p++) {
		if(strcmp(end, p->ext) == 0)
			return p->mime;
	}
	return NULL;
}

void *thread(void *fd_p)
{
	int fd = *(int*)fd_p;

	while(1) {
		int c, f;
		struct request req = {0};
		struct stat buf;
		char *url, *tmp, *http;
		const char *mime;
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
			http_error(c, 501, "HTTP/1.0");
			close(c);
			continue;
		}

		url = get_path(&req, &(char*){0}, &http);
		if(!url) {
			http_error(c, 400, http);
			close(c);
			continue;
		}

		if((tmp = strstr(url, "/../"))) {
			http_error(c, 403, http);
			close(c);
			continue;
		}
		if((tmp = strrchr(url, '/'))) {
			if(strcmp(tmp, "/..") == 0) {
				http_error(c, 403, http);
				close(c);
				continue;
			}
		}
		for(tmp = url; *tmp == '/'; tmp++);
		if(strcmp(tmp, "") == 0) tmp = ".";
		mime = mime_type(tmp);
		f = open(tmp, O_RDONLY);
		if(f < 0) {
			switch(errno) {
			case ENOENT:
				http_error(c, 404, http);
				break;
			case EACCES:
				http_error(c, 403, http);
				break;
			case ENFILE:
				http_error(c, 503, http);
				break;
			default:
				http_error(c, 500, http);
				break;
			}
			close(c);
			continue;
		}

		if(fstat(f, &buf) < 0) {
			http_error(c, 500, http);
			close(c);
			close(f);
			continue;
		}

		if(S_ISDIR(buf.st_mode)) {
			int tmp_f = openat(f, "index.html", O_RDONLY);
			if(tmp_f >= 0) {
				if(fstat(tmp_f, &buf) < 0 || S_ISDIR(buf.st_mode)) {
					close(tmp_f);
					tmp_f = -1;
				} else {
					mime = "text/html; charset=UTF-8";
					close(f);
					f = tmp_f;
				}
			}

			if(tmp_f < 0) {
				do_dir_list(tmp, c, f, http);
				close(c);
				continue;
			}
		}

		if(strcmp(http, "HTTP/0.9") != 0) {
			char date[50], mtime[50], expires[50];
			time_t cur;
			time_t exp;
			struct tm tmp;
			cur = time(0);
			gmtime_r(&cur, &tmp);
			strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tmp);
			gmtime_r(&buf.st_mtim.tv_sec, &tmp);
			strftime(mtime, sizeof mtime, "%a, %d %b %Y %H:%M:%S GMT", &tmp);
			// expire in a day; just a guess for static files
			exp = cur + 60*60*24;
			gmtime_r(&exp, &tmp);
			strftime(expires, sizeof expires, "%a, %d %b %Y %H:%M:%S GMT", &tmp);

			if(dprintf(c, "HTTP/1.0 200 OK\r\n") < 0)
				syslog(LOG_ERR, "dprintf: %m");
			if(mime)
				if(dprintf(c, "Content-Type: %s\r\n", mime) < 0)
					syslog(LOG_ERR, "dprintf: %m");
			if(dprintf(c, "Content-Length: %jd\r\n"
				       "Date: %s\r\n"
				       "Last-Modified: %s\r\n"
				       "Expires: %s\r\n\r\n",
				       (intmax_t)buf.st_size, date, mtime,
				       expires) < 0)
				syslog(LOG_ERR, "dprintf: %m");
		}

		while((n = sendfile(c, f, 0, 500 * 1024 * 1024)) > 0);
		if(n < 0) syslog(LOG_ERR, "sendfile: %m");
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
	setsockopt(fd, SOL_TCP, TCP_FASTOPEN, (int[]){100}, sizeof(int));
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
