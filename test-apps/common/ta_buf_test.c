#include "ta_buf.h"
#include <assert.h>
#include <string.h>

int main(void) {
    char a[TA_JOIN_MAX], b[64];
    assert(ta_copy(a, sizeof a, "/tmp") == 4 && !strcmp(a, "/tmp"));
    assert(ta_join(b, sizeof b, "/tmp", "x.txt") == strlen("/tmp/x.txt"));
    assert(!strcmp(b, "/tmp/x.txt"));
    assert(ta_join_suf(b, sizeof b, "/tmp", "f", ".trashinfo") == strlen("/tmp/f.trashinfo"));
    assert(!strcmp(b, "/tmp/f.trashinfo"));
    assert(ta_fmt(b, sizeof b, "%d-%s", 7, "hi") == 4 && !strcmp(b, "7-hi"));
    /* truncation flag */
    char small[4];
    assert(!ta_truncated());
    ta_copy(small, sizeof small, "0123456789");
    assert(ta_truncated());
    assert(!strcmp(small, "012"));
    /* NULL safety */
    ta_copy(b, sizeof b, NULL);
    assert(b[0] == '\0');
    printf("ta_buf: all checks passed\n");
    return 0;
}
