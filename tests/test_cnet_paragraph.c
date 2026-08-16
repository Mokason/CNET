#include "../include/cnet_paragraph.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define REQUIRE(cond, reason)                                                 \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("CNET_PARAGRAPH_RED reason=%s\n", reason);                 \
            ++failures;                                                       \
            return;                                                           \
        }                                                                     \
    } while (0)

static void test_lookup_and_paragraph(void) {
    CnetRestRow row;
    char para[CNET_PARA_TEXT];
    REQUIRE(cnet_rest_lookup("centre", "italian", "moderate", &row) == 0,
            "unique");
    REQUIRE(strcmp(row.name, "Cotto") == 0, "name");
    REQUIRE(cnet_rest_paragraph(&row, para, sizeof para) == 0, "para");
    REQUIRE(cnet_para_slottrue_v1(para, &row), "rubric");
    REQUIRE(strstr(para, "Cotto") && strstr(para, "centre") &&
                strstr(para, "italian") && strstr(para, "moderate"),
            "slots");
}

static void test_unique_or_abstain(void) {
    CnetRestRow row;
    REQUIRE(cnet_rest_lookup("centre", "italian", NULL, &row) == 1,
            "ambiguous");
    REQUIRE(cnet_rest_lookup("centre", "italian", "cheap", &row) == 1,
            "no_row");
    REQUIRE(cnet_rest_lookup(NULL, NULL, NULL, &row) == 1, "empty");
    REQUIRE(cnet_rest_lookup("north", "chinese", "cheap", &row) == 0 &&
                strcmp(row.name, "North Garden") == 0,
            "north");
}

static void test_wrap_then_paragraph(void) {
    CnetRestRow query, row;
    char para[CNET_PARA_TEXT];
    REQUIRE(cnet_rest_from_wrap(
                "looking for moderate italian in the centre please", &query) ==
                0,
            "wrap");
    REQUIRE(strcmp(query.area, "centre") == 0 &&
                strcmp(query.food, "italian") == 0 &&
                strcmp(query.price, "moderate") == 0,
            "slots_from_wrap");
    REQUIRE(cnet_rest_lookup(query.area, query.food, query.price, &row) == 0,
            "lookup");
    REQUIRE(cnet_rest_paragraph(&row, para, sizeof para) == 0, "para");
    REQUIRE(cnet_para_slottrue_v1(para, &row), "rubric");
}

static void test_ood_abstain(void) {
    CnetRestRow query, row;
    char para[CNET_PARA_TEXT];
    REQUIRE(cnet_rest_from_wrap("write me a love poem about mars", &query) == 1,
            "poem");
    REQUIRE(cnet_rest_from_wrap("martian food in the centre", &query) == 0,
            "partial");
    REQUIRE(cnet_rest_lookup(query.area, query.food, query.price, &row) == 1,
            "martian_unbound");
    memset(&row, 0, sizeof row);
    REQUIRE(cnet_rest_paragraph(&row, para, sizeof para) != 0, "no_unbound_para");
    REQUIRE(cnet_para_slottrue_v1("One line only.", &row) == 0, "short");
}

/* The rubric must discriminate, not wave everything through. Each case below
   is well-formed prose about the right row except for one broken axis. */
static void test_rubric_discriminates(void) {
    CnetRestRow row;
    REQUIRE(cnet_rest_lookup("centre", "italian", "moderate", &row) == 0,
            "cotto");

    /* Prose we did not generate, but slot-true, must pass: the rubric checks
       slot truth, not string identity with cnet_rest_paragraph. */
    REQUIRE(cnet_para_slottrue_v1(
                "Cotto serves italian in the centre. It is moderate.", &row),
            "other_true_prose");

    /* Wrong value on one slot at a time. */
    REQUIRE(cnet_para_slottrue_v1(
                "The match is Cotto. It is in the north and serves italian. "
                "The price range is moderate.",
                &row) == 0,
            "wrong_area");
    REQUIRE(cnet_para_slottrue_v1(
                "The match is Cotto. It is in the centre and serves chinese. "
                "The price range is moderate.",
                &row) == 0,
            "wrong_food");
    REQUIRE(cnet_para_slottrue_v1(
                "The match is Cotto. It is in the centre and serves italian. "
                "The price range is expensive.",
                &row) == 0,
            "wrong_price");
    REQUIRE(cnet_para_slottrue_v1(
                "The match is Piazza. It is in the centre and serves italian. "
                "The price range is moderate.",
                &row) == 0,
            "wrong_name");

    /* One sentence is not a paragraph, even when every slot is true. */
    REQUIRE(cnet_para_slottrue_v1(
                "Cotto is a moderate italian place in the centre.", &row) == 0,
            "one_sentence");

    /* Residual markers are refused even when the slots are true. */
    REQUIRE(cnet_para_slottrue_v1(
                "The match is Cotto. It is in the centre and serves italian. "
                "The price range is moderate. The teacher said so.",
                &row) == 0,
            "residual_marker");
}

static void test_no_residual(void) {
    CnetRestRow row;
    char para[CNET_PARA_TEXT];
    REQUIRE(cnet_rest_lookup("west", "indian", "expensive", &row) == 0, "raj");
    REQUIRE(cnet_rest_paragraph(&row, para, sizeof para) == 0, "para");
    REQUIRE(strstr(para, "llm") == NULL && strstr(para, "teacher") == NULL &&
                strstr(para, "Bonsai") == NULL,
            "residual");
}

int main(void) {
    test_lookup_and_paragraph();
    test_unique_or_abstain();
    test_wrap_then_paragraph();
    test_ood_abstain();
    test_rubric_discriminates();
    test_no_residual();
    if (failures != 0) return 1;
    printf("CNET_PARAGRAPH_PASS contract=%s rubric=%s sentences>=2 "
           "ood_abstain=1 residual=0 broader_claims=WITHHELD\n",
           CNET_PARA_CONTRACT, CNET_PARA_RUBRIC);
    return 0;
}
