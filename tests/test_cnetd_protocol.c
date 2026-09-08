/* cnetd request-boundary regression gate.
 * RED marker: CNETD_PROTOCOL_BOUNDARY_RED
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../include/cnetd_protocol.h"

static int fails;

static void check(int ok, const char *name) {
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

int main(void) {
    CnetdRequest req;
    char error[128];

    check(cnetd_parse_request(
              " { \"extra\": {\"nested\":[1,true,null]}, \"q\" : \"say \\\"hi\\\"\\nnow\", \"op\" : \"ask\" } ",
              &req, error, sizeof error) == 0,
          "strict JSON accepts documented request plus unknown fields");
    check(req.kind == CNETD_REQUEST_ASK && req.json,
          "JSON request has ASK kind and JSON response mode");
    check(strcmp(req.query, "say \"hi\"\nnow") == 0,
          "JSON string escapes are decoded exactly");

    check(cnetd_parse_request("PEER hermes who are you", &req,
                              error, sizeof error) == 0 &&
              strcmp(req.peer, "hermes") == 0,
          "PEER request captures request-local identity");
    check(cnetd_parse_request("{\"q\":\"who are you\",\"peer\":\"discord_fixture\"}", &req,
                              error, sizeof error) == 0 && !strcmp(req.peer, "discord_fixture"),
          "JSON peer identity survives the native adapter");
    check(cnetd_parse_request("{\"q\":\"x\",\"peer\":\"a\",\"peer\":\"b\"}", &req,
                              error, sizeof error) != 0,
          "duplicate JSON peer identity refuses");
    check(cnetd_parse_request("{\"q\":\"x\",\"peer\":\"a\\nb\"}", &req,
                              error, sizeof error) != 0,
          "JSON peer cannot carry line framing controls");
    check(cnetd_parse_request("{\"q\":\"who are you\"}", &req,
                              error, sizeof error) == 0 && req.peer[0] == '\0',
          "JSON request clears identity from the previous parse");
    check(cnetd_parse_request("raw local question", &req,
                              error, sizeof error) == 0 && req.peer[0] == '\0',
          "plain request also has local request identity");

    check(cnetd_parse_request("{\"x\":\"\\\"q\\\":\\\"forged\\\"\"}",
                              &req, error, sizeof error) != 0,
          "q-like text inside a string is not treated as a field");
    check(cnetd_parse_request("{\"q\":\"one\",\"q\":\"two\"}",
                              &req, error, sizeof error) != 0,
          "duplicate q fields are rejected as ambiguous");
    check(cnetd_parse_request("{\"op\":\"delete\",\"q\":\"x\"}",
                              &req, error, sizeof error) != 0,
          "unsupported JSON operations are rejected");
    check(cnetd_parse_request("{\"q\":\"x\"} trailing",
                              &req, error, sizeof error) != 0,
          "trailing JSON garbage is rejected");
    check(cnetd_parse_request("{\"q\":\"unterminated}",
                              &req, error, sizeof error) != 0,
          "unterminated JSON string is rejected");

    {
        int sv[2];
        char line[16];
        const char overlong[] = "ASK 0123456789abcdef";
        int pair_ok = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
        check(pair_ok, "create overlong framing pair");
        if (!pair_ok) goto after_overlong;
        if (write(sv[1], overlong, sizeof overlong - 1) > 0) {
            check(cnetd_read_request(sv[0], line, sizeof line, 200) ==
                      CNETD_READ_TOO_LONG,
                  "overlong request is rejected, never truncated");
        }
        close(sv[0]);
        close(sv[1]);
after_overlong:;
    }

    {
        int sv[2];
        char line[32];
        double start, elapsed;
        int pair_ok = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
        check(pair_ok, "create stalled framing pair");
        if (!pair_ok) goto after_stalled;
        start = now_ms();
        check(cnetd_read_request(sv[0], line, sizeof line, 60) ==
                  CNETD_READ_TIMEOUT,
              "stalled request hits the configured deadline");
        elapsed = now_ms() - start;
        check(elapsed >= 30.0 && elapsed < 500.0,
              "stalled request deadline is bounded in wall time");
        close(sv[0]);
        close(sv[1]);
after_stalled:;
    }

    {
        int sv[2];
        char line[32];
        int pair_ok = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
        check(pair_ok, "create complete framing pair");
        if (!pair_ok) goto after_complete;
        check(write(sv[1], "PING\r\n", 6) == 6,
              "write a complete CRLF request");
        check(cnetd_read_request(sv[0], line, sizeof line, 200) ==
                  CNETD_READ_OK && strcmp(line, "PING") == 0,
              "complete framed request is returned without CRLF");
        close(sv[0]);
        close(sv[1]);
after_complete:;
    }

    if (fails) {
        printf("CNETD_PROTOCOL_BOUNDARY_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("CNETD_PROTOCOL_BOUNDARY_PASS strict_json bounded_framing request_local_peer\n");
    return 0;
}
