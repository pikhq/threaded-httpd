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
#include <inttypes.h>

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
		char *msg;
	} *msg, msgs[] = {
		{400, "Bad Request"},
		{403, "Forbidden"},
		{404, "Not Found"},
		{413, "Request Entity Too Large"},
		{414, "Request-URI Too Long"},
		{416, "Requested Range Not Satisfiable"},
		{500, "Internal Server Error"},
		{501, "Not Implemented"},
		{503, "Service Unavailable"},
		{}
	};
	char buf[215];
	size_t len;

	for(msg = msgs; msg->c && msg->c != code; msg++);
	len = snprintf(buf, sizeof buf,
		"<!DOCTYPE html>\n"
		"<title>%d %s</title>\n"
		"<h1>%d Error</h1>\n"
		"%s\n",
		code, msg->c ? msg->msg : "error",
		code,
		msg->c ? msg->msg : "An error occured in the request.");
	if(len > sizeof buf)
		len = snprintf(buf, sizeof buf, "An error occured in the request.");

	if(strcmp(http, "HTTP/0.9") != 0)
		dprintf(fd, "%s %d %s\r\n"
			    "Content-type: text/html; charset=UTF-8\r\n"
			    "Content-Length: %d\r\n"
			    "\r\n",
			    http, code, msg->c ? msg->msg : "Error", len);
	dprintf(fd, "%s", buf);
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

static void url_enc(char *s, int f, int chunk)
{
	for(; *s; s++) {
		char *c = (char[4]){ *s, 0 };
		if(!isalnum(*s) && *s != '-' && *s != '_' && *s != '.' && *s != '~' && *s != '/')
			sprintf(c, "%%%.2X", (unsigned)*s);
		if(chunk)
			dprintf(f, "%x\r\n%s\r\n", (int)strlen(c), c);
		else
			dprintf(f, "%s", c);
	}
}

static void do_dir_list(char *s, int c, int f, char *http)
{
	DIR *d = fdopendir(f);
	struct dirent *de, **names=0, **tmp;
	size_t cnt=0, len=0;
	int chunk = 0;

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

	if(strcmp(http, "HTTP/1.1") == 0)
		chunk = 1;
	dprintf(c, "%s<!DOCTYPE html>\r\n%s", chunk ? "11\r\n" : "",
			chunk ? "\r\n" : "");
	for(size_t i = 0; i < cnt; i++) {
		dprintf(c, "%s<a href=\"/%s", chunk ? "a\r\n" : "",
				chunk ? "\r\n" : "");
		url_enc(s, c, chunk);
		dprintf(c, "%s/%s", chunk ? "1\r\n" : "",
				chunk ? "\r\n" : "");
		url_enc(names[i]->d_name, c, chunk);
		dprintf(c, "%s\">%s", chunk ? "2\r\n" : "",
				chunk ? "\r\n" : "");
		for(size_t j = 0; names[i]->d_name[j]; j++) {
			switch(names[i]->d_name[j]) {
			case '<':
				dprintf(c, "%s&lt;%s", chunk ? "4\r\n" : "",
						chunk ? "\r\n" : "");
				break;
			case '>':
				dprintf(c, "%s&gt;%s", chunk ? "4\r\n" : "",
						chunk ? "\r\n" : "");
				break;
			case '&':
				dprintf(c, "%s&amp;%s", chunk ? "5\r\n" : "",
						chunk ? "\r\n" : "");
				break;
			case '"':
				dprintf(c, "%s&quot;%s", chunk ? "6\r\n" : "",
						chunk ? "\r\n" : "");
				break;
			case '\'':
				dprintf(c, "%s&apos;%s", chunk ? "6\r\n" : "",
						chunk ? "\r\n" : "");
				break;
			default:
				dprintf(c, "%s%c%s", chunk ? "1\r\n" : "", names[i]->d_name[j],
						chunk ? "\r\n" : "");
			}
		}
		dprintf(c, "%s</a><br/>\r\n%s", chunk ? "b\r\n" : "",
				chunk ? "\r\n" : "");
		free(names[i]);
	}
	if(chunk)
		dprintf(c, "0\r\n\r\n");

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

	in++;
	*http = in;
	for(; *in != '\r' && *in != '\n'; in++);
	*in = '\0';
	if(strcmp(*http, "") == 0) *http = "HTTP/0.9";

	req->cur += in - (req->buf + req->cur);
	req->cur++;
	if(req->buf[req->cur] == '\n') req->cur++;

	*out = '\0';
	return ret;
}

static int get_header(struct request *req, char **head, char **value)
{
	char *head_end;
	if(req->buf[req->cur] == '\r') {
		*head = 0;
		*value = 0;
		req->cur += 2;
		return 0;
	}
	*head = req->buf + req->cur;
	head_end = strchr(req->buf + req->cur, ':');
	if(!head_end) return -1;
	req->cur += head_end - (req->buf + req->cur);
	req->buf[req->cur] = '\0';
	for(; req->buf[req->cur] != ' ' && req->buf[req->cur] != '\r' && req->buf[req->cur] != '\n'; req->cur++);
	req->cur++;
	*value = req->buf + req->cur;
	for(; req->buf[req->cur] != '\r' && req->buf[req->cur] != '\n'; req->cur++);
	req->buf[req->cur] = '\0';
	req->cur++;
	if(req->buf[req->cur] == '\n') req->cur++;

	return 0;
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
		char *url, *tmp, *http = "HTTP/1.0";
		char *head, *val;
		const char *mime;
		ssize_t n;
		int method;
		int close_conn = 1;
		time_t if_mod = 0;
		off_t start_off, num_bytes, total_size;

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

		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (int[]){15}, sizeof(int));

	next_req:
		if_mod = 0;
		start_off = 0;
		num_bytes = -1;
		total_size = -1;
		if(read_request(c, &req) < 0) {
			close(c);
			continue;
		}
		switch((method = get_method(&req))) {
		case HTTP_GET:
			break;
		case HTTP_HEAD:
			break;
		default:
			http_error(c, 501, http);
			goto exit_loop;
		}

		url = get_path(&req, &(char*){0}, &http);
		if(!url) {
			http_error(c, 400, http);
			goto exit_loop;
		}
		if(strcmp(http, "HTTP/1.1") == 0)
			close_conn = 0;

		do {
			if(get_header(&req, &head, &val) < 0) {
				http_error(c, 400, http);
				goto exit_loop;
			}
			if(head && strcmp(head, "Connection") == 0) {
				if(strcmp(http, "HTTP/1.1") == 0
				  && strcmp(val, "close") == 0)
					close_conn = 1;
			}
			if(head && strcmp(head, "If-Modified-Since") == 0) {
				char *end;
				struct tm tmp;
				end = strptime(val, "%a, %d %b %Y %H:%M:%S GMT", &tmp);
				if(!end || *end)
					end = strptime(val, "%A, %d-%b-%y %H:%M:%S GMT", &tmp);
				if(!end || *end)
					end = strptime(val, "%a %b %d %H:%M:%S %Y", &tmp);
				if(end && !*end) {
					if_mod = timegm(&tmp);
				}
			}
			if(head && strcmp(head, "Range") == 0) {
				char *end = val;
				uintmax_t start_range, end_range, total_size;
				if(strncmp("bytes=", end, 6) != 0) goto end_range_parse;
				end += 6;
				errno = 0;
				start_range = strtoumax(end, &end, 10);
				if(start_range == UINTMAX_MAX && errno == ERANGE)
					goto end_range_parse;
				if(start_range == 0 && errno == EINVAL)
					goto end_range_parse;
				if(*end != '-') goto end_range_parse;
				end++;
				if(isdigit(*end)) {
					end_range = strtoumax(end, &end, 10);
					if(end_range == UINTMAX_MAX && errno == ERANGE)
						goto end_range_parse;
					if(end_range < start_range)
						goto end_range_parse;
					if(*end == '/') end++;
				} else if(*end == '/') {
					end++;
					end_range = -1;
				} else if(*end) {
					goto end_range_parse;
				}

				if(*end) {
					total_size = strtoumax(end, &end, 10);
					if(total_size == UINTMAX_MAX && errno == ERANGE)
						goto end_range_parse;
					if(total_size == 0 && errno == EINVAL) {
						if(*end == '*')
							total_size = -1;
						else
							goto end_range_parse;
					}
				} else {
					total_size = -1;
				}
				start_off = start_range;
				if(end_range)
					num_bytes = (end_range - start_range) + 1;
			end_range_parse:
				;
			}
		} while(head && val);

		if((tmp = strstr(url, "/../"))) {
			http_error(c, 403, http);
			goto exit_loop;
		}
		if((tmp = strrchr(url, '/'))) {
			if(strcmp(tmp, "/..") == 0) {
				http_error(c, 403, http);
				goto exit_loop;
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
			goto exit_loop;
		}

		if(fstat(f, &buf) < 0) {
			http_error(c, 500, http);
			close(f);
			goto exit_loop;
		}

		if(S_ISDIR(buf.st_mode)) {
			struct stat tmp_buf;
			int tmp_f = openat(f, "index.html", O_RDONLY);
			if(tmp_f >= 0) {
				if(fstat(tmp_f, &tmp_buf) < 0 || S_ISDIR(tmp_buf.st_mode)) {
					close(tmp_f);
					tmp_f = -1;
				} else {
					mime = "text/html; charset=UTF-8";
					buf = tmp_buf;
					close(f);
					f = tmp_f;
				}
			}

			if(tmp_f < 0) {
				mime = "text/html; charset=UTF-8";
				start_off = 0;
				num_bytes = -1;
				total_size = -1;
			}
		}

		if(num_bytes != -1) {
			if(start_off + num_bytes > buf.st_size) {
				http_error(c, 416, http);
				goto exit_loop;
			}
		} else {
			num_bytes = buf.st_size - start_off;
		}

		if(total_size != -1 && total_size > buf.st_size) {
			http_error(c, 416, http);
			goto exit_loop;
		}

		if(strcmp(http, "HTTP/0.9") != 0) {
			char date[50], mtime[50], expires[50];
			char *resp = "200 OK";
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

			if(if_mod && if_mod >= buf.st_mtim.tv_sec) {
				resp = "304 Not Modified";
			} else if(start_off > 0 || num_bytes < buf.st_size) {
				resp = "206 Partial Content";
			}

			if(dprintf(c, "%s %s\r\n", http, resp) < 0)
				syslog(LOG_ERR, "dprintf: %m");
			if(mime)
				if(dprintf(c, "Content-Type: %s\r\n", mime) < 0)
					syslog(LOG_ERR, "dprintf: %m");
			if(S_ISDIR(buf.st_mode) && strcmp(http, "HTTP/1.1") == 0) {
				if(dprintf(c, "Transfer-Encoding: chunked\r\n") < 0)
					syslog(LOG_ERR, "dprintf: %m");
			}
			if(start_off > 0 || num_bytes < buf.st_size) {
				if(dprintf(c, "Content-Range: bytes %jd-%jd/%jd\r\n",
						(intmax_t)start_off,
						(intmax_t)(start_off + num_bytes - 1),
						(intmax_t)buf.st_size) < 0)
					syslog(LOG_ERR, "dprintf: %m");
				lseek(f, start_off, SEEK_SET);
			}
			if(dprintf(c, "Content-Length: %jd\r\n"
				      "Date: %s\r\n"
				      "Accept-Ranges: bytes\r\n"
				      "Last-Modified: %s\r\n"
				      "Expires: %s\r\n\r\n",
				      (intmax_t)buf.st_size, date, mtime,
				      expires) < 0)
				syslog(LOG_ERR, "dprintf: %m");
		}

		if(method != HTTP_HEAD || !if_mod ) {
			if(!S_ISDIR(buf.st_mode)) {
				while((n = sendfile(c, f, 0, num_bytes)) > 0) {
					num_bytes -= n;
					if(num_bytes == 0) break;
				}
				if(n < 0) syslog(LOG_ERR, "sendfile: %m");
			} else {
				do_dir_list(tmp, c, f, http);
			}
		}
		close(f);

exit_loop:
		if(close_conn) {
			close(c);
		} else {
			memmove(req.buf, req.buf + req.cur, req.buf_i - req.cur);
			req.buf_i = 0;
			req.cur = 0;
			goto next_req;
		}
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
		.ai_family = AF_INET6,
		.ai_socktype = SOCK_STREAM,
		.ai_flags = AI_V4MAPPED | AI_PASSIVE
	};
	struct addrinfo *res;

	signal(SIGPIPE, SIG_IGN);

	if((status = getaddrinfo(NULL, "8080", &hints, &res)) != 0) {
		if(status == EAI_ADDRFAMILY) {
			hints.ai_family = AF_INET;
			status = getaddrinfo(NULL, "8080", &hints, &res);
		}
	}

	if(status) {
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
