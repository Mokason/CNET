#include "cnet_chat_fluency.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    CnetChatFluencyQuery query;
    CnetChatFluencyScore score;
    CnetChatFact prior;
    int rc = 1;

#define REQUIRE(condition, reason)                                            \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_CHAT_FLUENCY_V1_RED reason=%s\n", reason);           \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    memset(&query, 0, sizeof query);
    query.answered = 1;
    query.when = "increment";
    query.contract = "increment_mod256";
    query.input = "12";
    query.output = "13";
    cnet_chat_fluency_v1_score(
        "Marble computes increment_mod256 on 12 as 13.", &query, &score);
    REQUIRE(score.valid == 1, "valid");
    REQUIRE(cnet_chat_fluency_v1_pass(&score, 1.0f) == 1, "good_own_text");

    cnet_chat_fluency_v1_score("", &query, &score);
    REQUIRE(score.well_formed == 0.0f && score.overall < 1.0f, "empty");

    cnet_chat_fluency_v1_score(
        "Marble computes increment_mod256 on 12 as 99.", &query, &score);
    REQUIRE(score.stay_on_contract == 0.0f, "wrong_output");
    REQUIRE(cnet_chat_fluency_v1_pass(&score, 1.0f) == 0, "wrong_not_pass");

    cnet_chat_fluency_v1_score(
        "Email the increment_mod256 of 12 as 13.", &query, &score);
    REQUIRE(score.no_side_effect == 0.0f, "email_leak");

    memset(&prior, 0, sizeof prior);
    snprintf(prior.contract, sizeof prior.contract, "%s", "increment_mod256");
    snprintf(prior.input, sizeof prior.input, "%s", "12");
    snprintf(prior.output, sizeof prior.output, "%s", "13");
    query.prior = &prior;
    query.prior_count = 1;
    query.answered = 1;
    query.contract = "increment_mod256";
    query.input = "12";
    query.output = "14";
    cnet_chat_fluency_v1_score(
        "Marble computes increment_mod256 on 12 as 14.", &query, &score);
    REQUIRE(score.no_contradiction == 0.0f, "contradicts_prior");

    memset(&query, 0, sizeof query);
    query.answered = 0;
    query.when = "refuse";
    cnet_chat_fluency_v1_score(
        "Marble refuses to invent a seal or voice a teacher draft. "
        "The standing law is never self-cert.",
        &query, &score);
    REQUIRE(cnet_chat_fluency_v1_pass(&score, 1.0f) == 1, "refuse_ok");

    printf("CNET_CHAT_FLUENCY_V1_PASS name=%s axes=%d "
           "broader_claims=WITHHELD\n",
           CNET_CHAT_FLUENCY_V1_NAME, CNET_CHAT_FLUENCY_V1_AXES);
    rc = 0;
done:
    return rc;
}
