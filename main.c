#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "buffer.h"
#include "cache.h"
#include "connection.h"
#include "http.h"
#include "parse.h"
#include "wikigrab.h"

#define DEFAULT_PROG_NAME_MAX		512

#define __noret __attribute__((noreturn))
#define __ctor __attribute__((constructor))
#define __dtor __attribute__((destructor))

static char PROG_NAME[DEFAULT_PROG_NAME_MAX];

wiki_cache_t *http_hcache;
static http_header_t *cookie;

int SOCK_SET_FLAG_ONCE;
int SOCK_SSL_SET_FLAG_ONCE;

static void
__noret usage(int status)
{
	printf(
			"%s <link> [options]\n\n"
			"-Q              show HTTP request header(s)\n"
			"-S              show HTTP response headers(s)\n"
			"--open/-O       open article in text editor when done\n"
			"--txt           format article in plain text file (default)\n"
			"--xml           format article in XML\n"
			"--print/-P      print the parsed article to stdout\n"
			"--help/-h       display this information\n",
			PROG_NAME);

	exit(status);
}

static void
cache_cleanup(void)
{
	wiki_cache_clear_all(http_hcache);
	wiki_cache_destroy(http_hcache);
}

static void
get_runtime_options(int argc, char *argv[])
{
	int i;

	for (i = 1; i < argc; ++i)
	{
		if (!strcmp("--help", argv[i])
		|| !strcmp("-h", argv[i]))
		{
			usage(EXIT_SUCCESS);
		}
		else
		if (!strcmp("--xml", argv[i]))
		{
			set_option(OPT_FORMAT_XML);
			unset_option(OPT_FORMAT_TXT);
		}
		else
		if (!strcmp("--txt", argv[i]))
		{
			set_option(OPT_FORMAT_TXT);
			unset_option(OPT_FORMAT_XML);
		}
		else
		if (!strcmp("--open", argv[i])
		|| !strcmp("-O", argv[i]))
		{
			set_option(OPT_OPEN_FINISH);
		}
		else
		if (!strcmp("-Q", argv[i]))
		{
			set_option(OPT_REQ_HEADER);
		}
		else
		if (!strcmp("-S", argv[i]))
		{
			set_option(OPT_RES_HEADER);
		}
		else
		if (!strcmp("--print", argv[i])
		|| !strcmp("-P", argv[i]))
		{
			set_option(OPT_OUT_TTY);
		}
		else
		{
			continue;
		}
	}

	/*
	 * If none set, use default of txt
	 */
	if (!option_set(OPT_FORMAT_TXT|OPT_FORMAT_XML))
		set_option(OPT_FORMAT_TXT);
}

static int
check_wikigrab_dir(void)
{
	char *home;
	buf_t tmp_buf;

	home = getenv("HOME");

	if (buf_init(&tmp_buf, pathconf("/", _PC_PATH_MAX)) < 0)
		goto fail;
		
	buf_append(&tmp_buf, home);
	buf_append(&tmp_buf, WIKIGRAB_DIR);

	if (access(tmp_buf.buf_head, F_OK) != 0)
	{
		mkdir(tmp_buf.buf_head, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH);
	}

	buf_destroy(&tmp_buf);
	return 0;

	fail:
	return -1;
}

int
main(int argc, char *argv[])
{
	memset(PROG_NAME, 0, DEFAULT_PROG_NAME_MAX);
	strncpy(PROG_NAME, argv[0], DEFAULT_PROG_NAME_MAX);

	if (argc < 2)
		usage(EXIT_FAILURE);

	SOCK_SET_FLAG_ONCE = 0;
	SOCK_SSL_SET_FLAG_ONCE = 0;

	get_runtime_options(argc, argv);

	if (!strstr(argv[1], "/wiki/"))
	{
		fprintf(stderr,
			"Not a wiki link!\n\n");
		usage(EXIT_FAILURE);
	}

#if 0
	printf(
		"________________________________________\n"
		"\n"
		"          WikiGrab v%s\n"
		"\n"
		" Written by Gary Hannah\n"
		"________________________________________\n\n",
		WIKIGRAB_BUILD);
#endif

	if (check_wikigrab_dir() < 0)
		goto fail;

	connection_t conn;
	//buf_t read_copy;
	http_header_t *location;
	off_t off;
	int status_code;
	int exit_ret = EXIT_SUCCESS;
	int host_max;

	http_hcache = wiki_cache_create(
				"http_header_cache",
				sizeof(http_header_t),
				0,
				wiki_cache_http_cookie_ctor,
				wiki_cache_http_cookie_dtor);

	atexit(cache_cleanup);
	clear_struct(&conn);
	host_max = sysconf(_SC_HOST_NAME_MAX);

	if (!(conn.host = calloc(host_max, 1)))
	{
		fprintf(stderr, "main: calloc error (%s)\n", strerror(errno));
		goto fail;
	}

	if (!(conn.page = calloc(host_max, 1)))
	{
		fprintf(stderr, "main: calloc error (%s)\n", strerror(errno));
		goto fail;
	}

	if (!(http_parse_host(argv[1], conn.host)))
	{
		fprintf(stderr, "main: failed to parse host from URL\n");
		goto fail;
	}

	if (!(http_parse_page(argv[1], conn.page)))
	{
		fprintf(stderr, "main: failed to parse page from URL\n");
		goto fail;
	}

	if (open_connection(&conn) < 0)
		goto fail;

	again:
	buf_clear(&conn.write_buf);

	if (http_build_request_header(&conn, HTTP_GET, conn.page) < 0)
	{
		fprintf(stderr, "main: http_build_get_request_header error\n");
		goto fail_disconnect;
	}

	if (http_check_header(&conn.read_buf, "Set-Cookie", (off_t)0, &off))
	{
		off = 0;
		wiki_cache_clear_all(http_hcache);

		while (http_check_header(&conn.read_buf, "Set-Cookie", off, &off))
		{
			cookie = (http_header_t *)wiki_cache_alloc(http_hcache, &cookie);
			if (!(http_fetch_header(&conn.read_buf, "Set-Cookie", cookie, off)))
			{
				fprintf(stderr, "main: http_fetch_header error\n");
				goto fail_disconnect;
			}

			if (http_append_header(&conn.write_buf, cookie) < 0)
			{
				fprintf(stderr, "main: http_append_header error\n");
				goto fail_disconnect;
			}

			++off;
		}
	}

	buf_clear(&conn.read_buf);

	if (option_set(OPT_REQ_HEADER))
		fprintf(stdout, "%s", conn.write_buf.buf_head);

	if (http_send_request(&conn) < 0)
		goto fail_disconnect;

	if (http_recv_response(&conn) < 0)
		goto fail_disconnect;

	if (option_set(OPT_RES_HEADER))
		fprintf(stdout, "%.*s", (int)http_response_header_len(&conn.read_buf), conn.read_buf.buf_head);

	status_code = http_status_code_int(&conn.read_buf);

	switch(status_code)
	{
		case HTTP_OK:
			exit_ret = extract_wiki_article(&conn.read_buf);
			if (exit_ret < 0)
			{
				printf("main: extract_wiki_article error\n");
				goto fail_disconnect;
			}
			break;
		case HTTP_MOVED_PERMANENTLY:
			location = wiki_cache_alloc(http_hcache, &location);
			assert(wiki_cache_obj_used(http_hcache, (void *)location));

			if (!(http_fetch_header(&conn.read_buf, "Location", location, (off_t)0)))
			{	
				fprintf(stderr, "main: http_fetch_header error\n");
				goto fail_disconnect;
			}

			strncpy(conn.page, location->value, location->vlen);
			conn.page[location->vlen] = 0;
			if (!(http_parse_host(location->value, conn.host)))
			{
				fprintf(stderr, "main: failed to parse host from URL\n");
				goto fail_disconnect;
			}

#if 0
/*
 * Default is now to always use TLS
 */
			if (!strncmp("https", location->value, 5) && !option_set(OPT_USE_TLS))
			{
				buf_init(&read_copy, conn.read_buf.buf_size);
				buf_copy(&read_copy, &conn.read_buf);

				conn_switch_to_tls(&conn);

				buf_copy(&conn.read_buf, &read_copy);
				buf_destroy(&read_copy);
			}
#endif

			buf_clear(&conn.write_buf);
			wiki_cache_dealloc(http_hcache, (void *)location, &location);
			goto again;
		default:
			fprintf(stderr, "Error (HTTP status code: %d -- %s)\n", status_code, http_status_code_string(status_code));
			goto fail_disconnect;
	}

	free(conn.host);
	free(conn.page);
	close_connection(&conn);
	exit(EXIT_SUCCESS);

	fail_disconnect:
	fprintf(stderr, "Disconnecting from remote server\n");
	free(conn.host);
	free(conn.page);
	close_connection(&conn);

	fail:
	if (conn.host)
		free(conn.host);
	if (conn.page)
		free(conn.page);

	exit(EXIT_FAILURE);
}
