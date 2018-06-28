#define _XOPEN_SOURCE 700
#define  _BSD_SOURCE

#include <stdio.h>
#include <traildb.h>
#include <Judy.h>
#include <json-c/json.h>
#include <stdbool.h>
#include <string.h>

#include "safeio.h"

typedef uint64_t timestamp_t;

void print_trails(char **traildb_paths, int num_paths, Pvoid_t cookies) {
    printf("[");

    for (int ti = 0; ti < num_paths; ti++) {
        tdb *db = tdb_init();
        tdb_error res = tdb_open(db, traildb_paths[ti]);
        CHECK_TDB(res, "failed to open traildb %s", traildb_paths[ti]);

        int num_fields = tdb_num_fields(db);

        printf("{");

        int num_trails = tdb_num_trails(db);
        bool found_cookie = false;
        for (int i = 0; i < num_trails; i++) {
            const uint8_t *cookie = tdb_get_uuid(db, i);

            uint8_t hexcookie[33] = {0};
            tdb_uuid_hex(cookie, hexcookie);

            Word_t *pv;
            JSLG(pv, cookies, hexcookie);

            if (pv) {
                tdb_cursor *cursor = tdb_cursor_new(db);
                tdb_error e = tdb_get_trail(cursor, i);
                CHECK_TDB(e, "could not read trail %d", i);

                const tdb_event *event;
                if (!found_cookie) {
                    printf("\"%s\": ", (const char *)hexcookie);
                    found_cookie = true;
                } else {
                    printf(",\"%s\": ", (const char *)hexcookie);
                }

                printf("[");
                int ei = 0;
                while ((event = tdb_cursor_next(cursor))) {
                    struct json_object *jitem = json_object_new_object();

                    if (ei != 0) {
                        printf(",");
                    }

                    timestamp_t ts = (timestamp_t)event->timestamp;
                    json_object_object_add(jitem, "timestamp", json_object_new_int64(ts));

                    for (int k = 1; k < num_fields; k++) {
                        const char *field_name = tdb_get_field_name(db, k);
                        uint64_t len;
                        const char *field_value = tdb_get_item_value(db, event->items[k-1], &len);
                        if (field_value != NULL)
                            json_object_object_add(jitem, field_name, json_object_new_string_len(field_value, len));
                    }

                    printf("%s", json_object_to_json_string(jitem));
                    ei++;
                    json_object_put(jitem);
                }
                printf("]");
            }
        }

        if (ti == num_paths - 1) {
            printf("}");
        } else {
            printf("},");
        }

        tdb_close(db);
    }
    printf("]");
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "too few arguments\n");
        exit(1);
    }

    char *cookies = strdup(argv[1]);

    Pvoid_t cookie_set = NULL;
    Word_t *pv;

    char buf[50] = {0};

    if (strcmp(cookies, "-") == 0) {
        while (fgets(buf, sizeof(buf), stdin)) {
            if (strlen(buf) == 33) {
                buf[32] = 0;
                JSLI(pv, cookie_set, (uint8_t *)buf);
                *pv = 1;
            } else {
                fprintf(stderr, "invalid cookie: %s\n", buf);
                exit(1);
            }
        }
    } else {
        char *list = cookies;
        char *token;

        while ((token = strsep(&cookies, ",")) != NULL) {
            JSLI(pv, cookie_set, (uint8_t *)token);
            *pv = 1;
        }

        free(list);
    }

    print_trails(&argv[2], argc-2, cookie_set);
    Word_t bytes_freed;
    JSLFA(bytes_freed, cookie_set);
}
