/* own-learning health check — refuse dangerous unattended configurations.
 *
 * "Conditionally safe" is only worth anything if the conditions are checked by
 * something other than a human reading four bullet points. This inspects the
 * deployed profile and the on-disk state and exits non-zero when the
 * combination would let mined units answer outside what they were certified
 * for.
 *
 * STRICT DEPLOYED MODE IS THE DEFAULT, and that is a correction. The 2026-07-30
 * re-analysis reproduced this tool printing OWN_LEARNING_HEALTH_PASS with
 * "base_loaded":0 against a base path that does not exist — it certified an
 * uninspectable deployment as healthy. It also read any non-"0" string as the
 * coverage gate being ON and any non-"1" string as mining being OFF, so
 * `CNET_COVERAGE_ABSTAIN=off` reported the gate as on. A deployment-health
 * marker now requires that the deployment was actually inspected:
 *
 *   - the base must be named and must load;
 *   - required coverage state must exist and parse;
 *   - every boolean knob must be exactly "0" or "1";
 *   - a configured residual teacher must be reachable.
 *
 * --config-only inspects knob relationships alone. It is a legitimate thing to
 * want (checking a profile before deploying it), so it is kept — but it emits
 * CONFIG_ONLY_PASS and never the deployment marker, because it has not looked
 * at a deployment.
 *
 * Usage: cnet_own_learning_health [--env-file config/personal-ai.env]
 *                                 [--base <path.cnb>] [--config-only]
 * Prints one JSON object. Exit 0 = safe, 1 = dangerous, 2 = usage/IO error.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/base.h"
#include "../include/contract/contract.h"
#include "../include/hybrid_ai.h"
#include "../include/cnet_checkpoint_internal.h"
#include "../include/nn.h"

#define MAX_DANGER 16
#define PROBE_TIMEOUT_MS_DEFAULT 2000

/* A tiny KEY=VALUE reader for the deployed EnvironmentFile: last assignment
   wins and commented lines are ignored, matching systemd's own reading, so a
   knob re-set further down the file is what we report. */
static int env_file_get(const char *path, const char *key, char *out,
                        size_t cap) {
    FILE *fp = fopen(path, "r");
    char line[1024];
    size_t klen = strlen(key);
    int found = 0;
    if (!fp) return -1;
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        if (strncmp(p, key, klen) != 0 || p[klen] != '=') continue;
        {
            char *v = p + klen + 1, *e;
            e = v + strlen(v);
            while (e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
                e--;
            *e = '\0';
            snprintf(out, cap, "%s", v);
            found = 1;
        }
    }
    fclose(fp);
    return found ? 0 : 1;
}

/* Process env wins over the file, since that is what the service actually
   runs with when an operator overrides a knob. */
static void knob(const char *envfile, const char *key, char *out, size_t cap) {
    const char *e = getenv(key);
    out[0] = '\0';
    if (e && e[0]) {
        snprintf(out, cap, "%s", e);
        return;
    }
    if (envfile) (void)env_file_get(envfile, key, out, cap);
}

/* Booleans are exactly "0" or "1". Unset is fine (the caller supplies the
   default); anything else is a typo the operator must see, not a value to
   guess at. Returns 1 for "1", 0 for "0" or unset, -1 for garbage. */
static int strict_bool(const char *v) {
    if (!v || !v[0]) return 0;
    if (v[0] == '1' && v[1] == '\0') return 1;
    if (v[0] == '0' && v[1] == '\0') return 0;
    return -1;
}

static int file_readable(const char *path) {
    FILE *fp;
    if (!path || !path[0]) return 0;
    fp = fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

/* --- configured-teacher reachability -------------------------------------
 * A nonempty URL counted as "residual configured" no matter what was (not)
 * listening on it, so a dead teacher read as a healthy one. This is a plain
 * TCP connect with a short timeout: it sends no request and reads no body, so
 * it cannot perturb whatever is on the other end.
 *
 * Returns 0 reachable, -1 unreachable, -2 unparseable URL. */
static int probe_http(const char *url, int timeout_ms) {
    char host[256], port[16];
    const char *p = url, *slash, *colon;
    size_t hostlen;
    struct addrinfo hints, *res = NULL, *ai;
    int rc = -1;

    if (!url || !url[0]) return -2;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        snprintf(port, sizeof port, "80");
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        snprintf(port, sizeof port, "443");
    } else {
        return -2;
    }

    slash = strchr(p, '/');
    hostlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hostlen == 0 || hostlen >= sizeof host) return -2;
    memcpy(host, p, hostlen);
    host[hostlen] = '\0';

    /* Split an explicit port. IPv6 literals in brackets are not used by any
       deployed profile here; refuse rather than mis-parse them. */
    if (host[0] == '[') return -2;
    colon = strrchr(host, ':');
    if (colon) {
        if (colon == host || !colon[1]) return -2;
        snprintf(port, sizeof port, "%s", colon + 1);
        host[colon - host] = '\0';
    }

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) return -1;

    for (ai = res; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        int flags;
        if (fd < 0) continue;
        flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            close(fd);
            rc = 0;
            break;
        }
        if (errno == EINPROGRESS) {
            fd_set wset;
            struct timeval tv;
            int ready;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            ready = select(fd + 1, NULL, &wset, NULL, &tv);
            if (ready > 0) {
                int soerr = 0;
                socklen_t len = sizeof soerr;
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 &&
                    soerr == 0) {
                    close(fd);
                    rc = 0;
                    break;
                }
            }
        }
        close(fd);
    }
    freeaddrinfo(res);
    return rc;
}

static void json_escape(const char *in, char *out, size_t cap) {
    size_t n = 0;
    if (cap == 0) return;
    for (; in && *in && n + 2 < cap; in++) {
        if (*in == '"' || *in == '\\') {
            out[n++] = '\\';
            out[n++] = *in;
        } else if ((unsigned char)*in < 0x20) {
            out[n++] = ' ';
        } else {
            out[n++] = *in;
        }
    }
    out[n] = '\0';
}

int main(int argc, char **argv) {
    const char *envfile = NULL;
    const char *base_path = NULL;
    char mine_on_serve[64], coverage_abstain[64], res_http[256], res_gguf[256];
    char cov_path[576];
    char base_escaped[1024], mine_escaped[160], gate_escaped[160];
    CnetBase base;
    HybridAi h;
    const char *danger[MAX_DANGER];
    size_t ndanger = 0, i;
    size_t mined = 0, unguarded = 0, records = 0, unreadable_units = 0;
    const char *cov_state = "missing";
    const char *teacher_state = "none";
    int base_ok = 0, base_named = 0, mine_flag, gate_off, residual_cfg;
    int mine_bool, gate_bool, config_only = 0, probe_timeout;
    int a;

    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--env-file") && a + 1 < argc) envfile = argv[++a];
        else if (!strcmp(argv[a], "--base") && a + 1 < argc) base_path = argv[++a];
        else if (!strcmp(argv[a], "--config-only")) config_only = 1;
        else {
            fprintf(stderr,
                    "usage: %s [--env-file F] [--base B] [--config-only]\n",
                    argv[0]);
            return 2;
        }
    }
    if (!base_path) base_path = getenv("CNET_BASE_PATH");
    base_named = base_path && base_path[0];
    {
        const char *t = getenv("CNET_HEALTH_PROBE_TIMEOUT_MS");
        probe_timeout = t && t[0] ? atoi(t) : PROBE_TIMEOUT_MS_DEFAULT;
        if (probe_timeout < 1 || probe_timeout > 60000)
            probe_timeout = PROBE_TIMEOUT_MS_DEFAULT;
    }

    knob(envfile, "CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE", mine_on_serve,
         sizeof mine_on_serve);
    knob(envfile, "CNET_COVERAGE_ABSTAIN", coverage_abstain,
         sizeof coverage_abstain);
    knob(envfile, "CNET_RESIDUAL_HTTP", res_http, sizeof res_http);
    knob(envfile, "CNET_RESIDUAL_GGUF", res_gguf, sizeof res_gguf);

    mine_bool = strict_bool(mine_on_serve);
    gate_bool = strict_bool(coverage_abstain);
    /* A garbage knob is treated as its DANGEROUS reading while it is reported,
       so a typo can never be quietly resolved in the deployment's favour. */
    mine_flag = mine_bool == 1 || mine_bool < 0;
    gate_off = gate_bool == 0 && coverage_abstain[0] != '\0';
    if (gate_bool < 0) gate_off = 1;
    residual_cfg = (res_http[0] != '\0') || (res_gguf[0] != '\0');

    hybrid_ai_init(&h);
    cnb_init(&base);
    if (base_named && !config_only) {
        if (cnb_load(&base, base_path) == 0) {
            base_ok = 1;
            snprintf(cov_path, sizeof cov_path, "%s.coverage", base_path);
            {
                FILE *fp = fopen(cov_path, "r");
                if (!fp) {
                    cov_state = "missing";
                } else {
                    fclose(fp);
                    cov_state = hybrid_coverage_load(&h, cov_path) < 0
                                    ? "unreadable"
                                    : "ok";
                }
            }
            records = hybrid_coverage_count(&h);
            for (i = 0; i < base.unit_count; i++) {
                const char *nm = base.units[i].name;
                BinaryTransformNetwork btn;
                Contract ct;
                int bound = 0;
                if (!hybrid_unit_is_mined(nm)) continue;
                mined++;
                /* Ask the SAME question serving asks. `hybrid_coverage_has_unit`
                   answers "is this name mentioned", so a record that named the
                   unit but bound a different family, tag, port or dimension
                   made this gate report health while personal_ai's startup --
                   which binds the exact interface -- armed fail-closed for the
                   very same unit. A deployment PASS that disagrees with the
                   serving decision is worse than no gate. */
                memset(&btn, 0, sizeof btn);
                memset(&ct, 0, sizeof ct);
                if (cnb_get_unit(&base, nm, &btn, &ct) == 0) {
                    if (btn.input_port_count >= 1 && btn.output_port_count >= 1)
                        bound = hybrid_coverage_binds_unit(&h, nm,
                                                           btn.input_ports[0],
                                                           btn.output_ports[0],
                                                           btn.input_count);
                    if (bound) bound = checkpoint_rows_bind(&h, nm, &ct, btn.input_count);
                    contract_free(&ct);
                    btn_free(&btn);
                } else {
                    unreadable_units++;
                }
                if (!bound) unguarded++;
            }
        }
    }

    /* --- knob relationships (checked in both modes) --------------------- */
    if (mine_bool < 0 && ndanger < MAX_DANGER)
        danger[ndanger++] = "invalid_boolean_mine_on_serve";
    if (gate_bool < 0 && ndanger < MAX_DANGER)
        danger[ndanger++] = "invalid_boolean_coverage_abstain";
    /* The one-env-typo failure: mining on, guard off. */
    if (mine_flag && gate_off && ndanger < MAX_DANGER)
        danger[ndanger++] = "mine_on_serve_with_coverage_gate_off";
    /* Mining enabled with nothing to learn from — silently a no-op. */
    if (mine_flag && !residual_cfg && ndanger < MAX_DANGER)
        danger[ndanger++] = "mine_on_serve_without_residual";

    /* --- deployment state (strict mode only) ---------------------------- */
    if (!config_only) {
        if (!base_named && ndanger < MAX_DANGER) {
            danger[ndanger++] = "base_not_specified";
        } else if (!base_ok && ndanger < MAX_DANGER) {
            /* The reproduction from the re-analysis: a base that is absent or
               unreadable used to leave base_loaded=0 and still print PASS. */
            danger[ndanger++] = "base_not_loaded";
        }
        /* Sealed mined units whose guard is gone: serve refuses them
           (fail-closed), but the library is degraded and an operator must
           re-mine. */
        if (unguarded > 0 && ndanger < MAX_DANGER)
            danger[ndanger++] = "mined_units_without_bound_coverage";
        if (unreadable_units > 0 && ndanger < MAX_DANGER)
            danger[ndanger++] = "mined_units_unreadable";
        if (base_ok && strcmp(cov_state, "unreadable") == 0 &&
            ndanger < MAX_DANGER)
            danger[ndanger++] = "coverage_file_unreadable";
        /* Coverage is REQUIRED whenever the gate is meant to be doing work.
           A missing sidecar under an armed gate is not "nothing to check", it
           is an unenforced guard. */
        if (base_ok && !gate_off && strcmp(cov_state, "missing") == 0 &&
            ndanger < MAX_DANGER)
            danger[ndanger++] = "coverage_file_missing";

        /* A configured teacher that nothing answers on is not a teacher. */
        if (res_http[0]) {
            int probe = probe_http(res_http, probe_timeout);
            teacher_state = probe == 0 ? "reachable"
                          : probe == -2 ? "unparseable_url"
                                        : "unreachable";
            if (probe != 0 && ndanger < MAX_DANGER)
                danger[ndanger++] = "residual_http_unreachable";
        } else if (res_gguf[0]) {
            teacher_state = file_readable(res_gguf) ? "reachable"
                                                    : "unreadable_gguf";
            if (!file_readable(res_gguf) && ndanger < MAX_DANGER)
                danger[ndanger++] = "residual_gguf_unreadable";
        }
    }

    /* Knob values are operator-supplied strings and are echoed back so a typo
       is visible in the report; escape them rather than emitting broken JSON. */
    json_escape(base_named ? base_path : "", base_escaped,
                sizeof base_escaped);
    json_escape(mine_on_serve, mine_escaped, sizeof mine_escaped);
    json_escape(coverage_abstain, gate_escaped, sizeof gate_escaped);
    printf("{\"mode\":\"%s\",\"base\":\"%s\",\"base_loaded\":%d,"
           "\"mined_units\":%zu,"
           "\"coverage_records\":%zu,\"unguarded_mined\":%zu,"
           "\"unreadable_mined\":%zu,"
           "\"coverage_file\":\"%s\",\"mine_on_serve\":%d,"
           "\"mine_on_serve_raw\":\"%s\",\"coverage_abstain_raw\":\"%s\","
           "\"coverage_gate\":\"%s\",\"residual_configured\":%d,"
           "\"residual_teacher\":\"%s\",\"dangerous\":[",
           config_only ? "config_only" : "deployed", base_escaped, base_ok,
           mined, records, unguarded, unreadable_units, cov_state, mine_flag,
           mine_escaped, gate_escaped,
           gate_off ? "off" : "on", residual_cfg, teacher_state);
    for (i = 0; i < ndanger; i++)
        printf("%s\"%s\"", i ? "," : "", danger[i]);
    printf("],\"status\":\"%s\"}\n", ndanger ? "danger" : "ok");

    /* Two markers, two meanings. A config-only run has not inspected a
       deployment and must never be readable as deployment health. */
    if (config_only) {
        if (ndanger == 0)
            printf("CONFIG_ONLY_PASS knobs=checked deployment=not_inspected\n");
        else
            printf("CONFIG_ONLY_FAIL count=%zu deployment=not_inspected\n",
                   ndanger);
    } else {
        if (ndanger == 0)
            printf("OWN_LEARNING_HEALTH_PASS base_loaded=1 coverage=%s "
                   "teacher=%s\n", cov_state, teacher_state);
        else
            printf("OWN_LEARNING_HEALTH_FAIL count=%zu\n", ndanger);
    }

    hybrid_ai_free(&h);
    cnb_free(&base);
    return ndanger ? 1 : 0;
}
