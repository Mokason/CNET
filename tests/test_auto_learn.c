/* Auto-learn unit gate — no full gap_lane link.
 * make auto_learn → AUTO_LEARN_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_auto_learn.h"
#include "../include/router.h"

/* Stubs for unit gate (avoid linking full CNET). */
int port_set_tag(Port *port, const char *tag) {
    if (!port) return -1;
    if (!tag) { port->tag[0] = '\0'; return 0; }
    snprintf(port->tag, sizeof port->tag, "%s", tag);
    return 0;
}

int gap_inbox_note_no_plan(const char *inbox_path, Port input_port, Port goal_port) {
    FILE *f;
    if (!inbox_path) return -1;
    f = fopen(inbox_path, "a");
    if (!f) return -1;
    fprintf(f, "NO_PLAN %d %zu %zu %s %d %zu %zu %s\n",
            (int)input_port.family, input_port.field_width, input_port.field_count,
            input_port.tag[0] ? input_port.tag : "-",
            (int)goal_port.family, goal_port.field_width, goal_port.field_count,
            goal_port.tag[0] ? goal_port.tag : "-");
    fclose(f);
    return 0;
}

static int fails;
static void check(int ok, const char *n) {
    printf("  %-50s %s\n", n, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    Port in, goal;
    const char *inbox = "tmp_auto_learn.inbox";
    char line[512];
    FILE *f;

    setenv("CNET_AUTO_LEARN", "1", 1);
    setenv("CNET_AUTO_LEARN_W", "256", 1);
    setenv("CNET_AUTO_LEARN_K", "3", 1);
    remove(inbox);

    check(cnet_auto_learn_enabled() == 1, "auto_learn enabled");

    memset(&in, 0, sizeof in);
    memset(&goal, 0, sizeof goal);
    in.family = PORT_ONEHOT;
    in.field_width = 256;
    in.field_count = 1;
    port_set_tag(&in, "w_cur");
    goal.family = PORT_ONEHOT;
    goal.field_width = 256;
    goal.field_count = 1;
    port_set_tag(&goal, "hard_suite_novel_goal_xyz");

    check(cnet_auto_learn_make_teachable(&in, &goal, "hard_suite_novel_goal_xyz") == 1,
          "freeform rewritten");
    check(strcmp(in.tag, "w_cur") == 0, "input tag w_cur");
    check(strncmp(goal.tag, "tk", 2) == 0 && strchr(goal.tag, 'q') != NULL,
          "goal tag tk*q*");
    check(cnet_auto_learn_shape_ok(in, goal), "shape teachable");

    {
        Port in2 = in, g2 = goal;
        check(cnet_auto_learn_make_teachable(&in2, &g2, NULL) == 0,
              "already teachable is no-op");
    }

    check(cnet_auto_learn_note_text(inbox, "user asked about memory witness", 3) == 0,
          "note_text writes inbox");
    f = fopen(inbox, "r");
    check(f != NULL && fgets(line, sizeof line, f) != NULL, "inbox line");
    if (f) {
        check(strncmp(line, "NO_PLAN ", 8) == 0, "NO_PLAN prefix");
        check(strstr(line, "w_cur") != NULL, "has w_cur");
        check(strstr(line, "tk") != NULL, "has tk");
        fclose(f);
    }
    remove(inbox);

    setenv("CNET_AUTO_LEARN", "0", 1);
    check(cnet_auto_learn_enabled() == 0, "can disable");

    if (fails) {
        printf("AUTO_LEARN_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("AUTO_LEARN_PASS checks_ok\n");
    return 0;
}
