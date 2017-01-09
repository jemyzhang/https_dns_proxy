// Simple UDP-to-HTTPS DNS Proxy
//
// (C) 2016 Aaron Drew
//
// Intended for use with Google's Public-DNS over HTTPS service
// (https://developers.google.com/speed/public-dns/docs/dns-over-https)
#include <sys/socket.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <errno.h>
#include <grp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <ares.h>
#include <ev.h>
#include "dns_server.h"
#include "dns_poller.h"
#include "https_client.h"
#include "json_to_dns.h"
#include "options.h"
#include "logging.h"

// Holds app state required for dns_server_cb.
typedef struct {
  https_client_t *https_client;
  struct curl_slist *resolv;
  // currently only used for edns_client_subnet, if specified.
  const char *extra_request_args;
} app_state_t;

typedef struct {
  uint16_t tx_id;
  struct sockaddr_in raddr;
  dns_server_t *dns_server;
} request_t;

static void sigint_cb(struct ev_loop *loop, ev_signal *w, int revents) {
  ev_break(loop, EVBREAK_ALL);
}

static void sigpipe_cb(struct ev_loop *loop, ev_signal *w, int revents) {
  ELOG("Received SIGPIPE. Ignoring.");
}

static void https_resp_cb(void *data, unsigned char *buf, unsigned int buflen) {
  DLOG("buflen %u\n", buflen);
  if (buf == NULL) { // Timeout, DNS failure, or something similar.
    return;
  }
  request_t *req = (request_t *)data;
  if (req == NULL) {
    FLOG("data NULL");
  }
  char *bufcpy = (char *)calloc(1, buflen + 1);
  if (bufcpy == NULL) {
    FLOG("calloc");
  }
  memcpy(bufcpy, buf, buflen);

  DLOG("Received response for id %04x: %.*s", req->tx_id, buflen, bufcpy);

  const int obuf_size = 1500;
  char obuf[obuf_size];
  int r;
  if ((r = json_to_dns(req->tx_id, bufcpy,
                       (unsigned char *)obuf, obuf_size)) <= 0) {
    ELOG("Failed to decode JSON.");
  } else {
    dns_server_respond(req->dns_server, req->raddr, obuf, r);
  }
  free(bufcpy);
  free(req);
}

static void dns_server_cb(dns_server_t *dns_server, void *data,
                          struct sockaddr_in addr, uint16_t tx_id,
                          uint16_t flags, const char *name, int type) {
  app_state_t *app = (app_state_t *)data;

  DLOG("Received request for '%s' id: %04x, type %d, flags %04x", name, tx_id,
       type, flags);

  // Build URL
  int cd_bit = flags & (1 << 4);
  char *escaped_name = curl_escape(name, strlen(name));
  char url[1500] = "";
  snprintf(url, sizeof(url) - 1,
           "https://dns.google.com/resolve?name=%s&type=%d%s%s",
           escaped_name, type, cd_bit ? "&cd=true" : "",
           app->extra_request_args);
  curl_free(escaped_name);

  request_t *req = (request_t *)calloc(1, sizeof(request_t));
  req->tx_id = tx_id;
  req->raddr = addr;
  req->dns_server = dns_server;
  https_client_fetch(app->https_client, url, app->resolv, https_resp_cb, req);
}

static void dns_poll_cb(void *data, struct sockaddr_in *addr) {
  struct curl_slist **resolv = (struct curl_slist **)data;
  char buf[128] = "dns.google.com:443:";
  char *end = &buf[128];
  char *pos = buf + strlen(buf);
  ares_inet_ntop(AF_INET, addr, pos, end - pos);
  DLOG("Received new IP '%s'", pos);
  curl_slist_free_all(*resolv);
  *resolv = curl_slist_append(NULL, buf);
}

int main(int argc, char *argv[]) {
  struct Options opt;
  options_init(&opt);
  if (options_parse_args(&opt, argc, argv)) {
    options_show_usage(argc, argv);
    exit(1);
  }

  logging_init(opt.logfd, opt.loglevel);

  ILOG("System c-ares: %s", ares_version(NULL));
  ILOG("System libcurl: %s", curl_version());

  // Note: curl intentionally uses uninitialized stack variables and similar
  // tricks to increase it's entropy pool. This confuses valgrind and leaks
  // through to errors about use of uninitialized values in our code. :(
  curl_global_init(CURL_GLOBAL_DEFAULT);

  // Note: This calls ev_default_loop(0) which never cleans up.
  //       valgrind will report a leak. :(
  struct ev_loop *loop = EV_DEFAULT;

  https_client_t https_client;
  https_client_init(&https_client, &opt, loop);

  app_state_t app;
  app.https_client = &https_client;
  app.resolv = NULL;
  if (opt.edns_client_subnet[0]) {
    static char buf[200];
    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf)-1, "&edns_client_subnet=%s",
             opt.edns_client_subnet);
    app.extra_request_args = buf;
  } else {
    app.extra_request_args = "";
  }

  dns_server_t dns_server;
  dns_server_init(&dns_server, loop, opt.listen_addr, opt.listen_port,
                  dns_server_cb, &app);

  if (opt.daemonize) {
    if (setgid(opt.gid))
      FLOG("Failed to set gid.");
    if (setuid(opt.uid))
      FLOG("Failed to set uid.");
    // daemon() is non-standard. If needed, see OpenSSH openbsd-compat/daemon.c
    daemon(0, 0);
  }

  ev_signal sigpipe;
  ev_signal_init(&sigpipe, sigpipe_cb, SIGPIPE);
  ev_signal_start(loop, &sigpipe);

  ev_signal sigint;
  ev_signal_init(&sigint, sigint_cb, SIGINT);
  ev_signal_start(loop, &sigint);

  dns_poller_t dns_poller;
  dns_poller_init(&dns_poller, loop, opt.bootstrap_dns, opt.http_dns_server,
                  120 /* seconds */, dns_poll_cb, &app.resolv);

  ev_run(loop, 0);

  dns_poller_cleanup(&dns_poller);

  curl_slist_free_all(app.resolv);

  ev_signal_stop(loop, &sigint);
  dns_server_cleanup(&dns_server);
  https_client_cleanup(&https_client);

  ev_loop_destroy(loop);

  curl_global_cleanup();
  logging_cleanup();
  options_cleanup(&opt);

  return EXIT_SUCCESS;
}
