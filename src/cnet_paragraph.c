#include "../include/cnet_paragraph.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *name;
    const char *area;
    const char *food;
    const char *price;
} RestFact;

/* In-house CamRest-shaped table. Not harvested assistant prose. */
static const RestFact k_table[] = {
    {"Cotto", "centre", "italian", "moderate"},
    {"Piazza", "centre", "italian", "expensive"},
    {"River Cafe", "east", "italian", "cheap"},
    {"North Garden", "north", "chinese", "cheap"},
    {"East Noodle", "east", "chinese", "cheap"},
    {"Raj House", "west", "indian", "expensive"},
    {"Spice Walk", "south", "indian", "moderate"},
    {"Oak Room", "south", "british", "moderate"},
    {"Crown Inn", "north", "british", "cheap"},
    {"West Grill", "west", "british", "expensive"},
};

static void copy_slot(char *dst, size_t cap, const char *src) {
    size_t n;
    if (dst == NULL || cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int slot_eq(const char *have, const char *need) {
    if (need == NULL || need[0] == '\0') return 1;
    if (have == NULL) return 0;
    return strcmp(have, need) == 0;
}

int cnet_rest_lookup(const char *area, const char *food, const char *price,
                     CnetRestRow *row) {
    size_t i, hits = 0, found = 0;
    if (row == NULL) return -1;
    memset(row, 0, sizeof *row);
    if ((area == NULL || area[0] == '\0') && (food == NULL || food[0] == '\0') &&
        (price == NULL || price[0] == '\0'))
        return 1;
    for (i = 0; i < sizeof k_table / sizeof k_table[0]; ++i) {
        if (!slot_eq(k_table[i].area, area) || !slot_eq(k_table[i].food, food) ||
            !slot_eq(k_table[i].price, price))
            continue;
        hits++;
        found = i;
    }
    if (hits != 1) return 1;
    copy_slot(row->name, sizeof row->name, k_table[found].name);
    copy_slot(row->area, sizeof row->area, k_table[found].area);
    copy_slot(row->food, sizeof row->food, k_table[found].food);
    copy_slot(row->price, sizeof row->price, k_table[found].price);
    return 0;
}

static int has_word(const char *lower, const char *word) {
    const char *at;
    size_t n;
    if (lower == NULL || word == NULL) return 0;
    n = strlen(word);
    at = lower;
    while ((at = strstr(at, word)) != NULL) {
        int left_ok = (at == lower) || !isalnum((unsigned char)at[-1]);
        int right_ok = !isalnum((unsigned char)at[n]);
        if (left_ok && right_ok) return 1;
        at += n;
    }
    return 0;
}

int cnet_rest_from_wrap(const char *wrap, CnetRestRow *query) {
    char lower[256];
    size_t i, n;
    int found = 0;
    if (query == NULL) return -1;
    memset(query, 0, sizeof *query);
    if (wrap == NULL || wrap[0] == '\0') return 1;
    n = strlen(wrap);
    if (n >= sizeof lower) n = sizeof lower - 1u;
    for (i = 0; i < n; ++i) lower[i] = (char)tolower((unsigned char)wrap[i]);
    lower[n] = '\0';
    if (has_word(lower, "centre") || has_word(lower, "center")) {
        copy_slot(query->area, sizeof query->area, "centre");
        found = 1;
    } else if (has_word(lower, "north")) {
        copy_slot(query->area, sizeof query->area, "north");
        found = 1;
    } else if (has_word(lower, "south")) {
        copy_slot(query->area, sizeof query->area, "south");
        found = 1;
    } else if (has_word(lower, "east")) {
        copy_slot(query->area, sizeof query->area, "east");
        found = 1;
    } else if (has_word(lower, "west")) {
        copy_slot(query->area, sizeof query->area, "west");
        found = 1;
    }
    if (has_word(lower, "italian")) {
        copy_slot(query->food, sizeof query->food, "italian");
        found = 1;
    } else if (has_word(lower, "chinese")) {
        copy_slot(query->food, sizeof query->food, "chinese");
        found = 1;
    } else if (has_word(lower, "indian")) {
        copy_slot(query->food, sizeof query->food, "indian");
        found = 1;
    } else if (has_word(lower, "british")) {
        copy_slot(query->food, sizeof query->food, "british");
        found = 1;
    }
    if (has_word(lower, "cheap")) {
        copy_slot(query->price, sizeof query->price, "cheap");
        found = 1;
    } else if (has_word(lower, "moderate") || has_word(lower, "moderately")) {
        copy_slot(query->price, sizeof query->price, "moderate");
        found = 1;
    } else if (has_word(lower, "expensive")) {
        copy_slot(query->price, sizeof query->price, "expensive");
        found = 1;
    }
    return found ? 0 : 1;
}

int cnet_rest_paragraph(const CnetRestRow *row, char *out, size_t cap) {
    int written;
    if (out == NULL || cap == 0) return -1;
    out[0] = '\0';
    if (row == NULL || row->name[0] == '\0' || row->area[0] == '\0' ||
        row->food[0] == '\0' || row->price[0] == '\0')
        return -1;
    written = snprintf(out, cap,
                       "The match is %s. It is in the %s and serves %s. "
                       "The price range is %s.",
                       row->name, row->area, row->food, row->price);
    if (written < 0 || (size_t)written >= cap) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

static int sentence_count(const char *spoken) {
    int n = 0;
    size_t i;
    if (spoken == NULL) return 0;
    for (i = 0; spoken[i] != '\0'; ++i) {
        if (spoken[i] == '.') ++n;
    }
    return n;
}

int cnet_para_slottrue_v1(const char *spoken, const CnetRestRow *row) {
    if (spoken == NULL || spoken[0] == '\0' || row == NULL ||
        row->name[0] == '\0')
        return 0;
    if (!isupper((unsigned char)spoken[0])) return 0;
    if (sentence_count(spoken) < 2) return 0;
    if (strstr(spoken, row->name) == NULL) return 0;
    if (strstr(spoken, row->area) == NULL) return 0;
    if (strstr(spoken, row->food) == NULL) return 0;
    if (strstr(spoken, row->price) == NULL) return 0;
    if (strstr(spoken, "llm") != NULL || strstr(spoken, "teacher") != NULL ||
        strstr(spoken, "Bonsai") != NULL || strstr(spoken, "  ") != NULL)
        return 0;
    return 1;
}
