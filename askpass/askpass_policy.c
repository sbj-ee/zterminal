#include "askpass_policy.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* ssh truncates the labels of password prompts: "%.30s@%.128s's password: ". */
#define SSH_USER_TRUNC 30
#define SSH_HOST_TRUNC 128

static int lower(int c)
{
    return tolower((unsigned char)c);
}

static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; ++hay) {
        size_t i = 0;
        while (i < n && hay[i] && lower(hay[i]) == lower(needle[i]))
            ++i;
        if (i == n)
            return 1;
    }
    return 0;
}

/* The label ssh printed matches `want` (maybe truncated by ssh to `trunc`). */
static int label_matches(const char *p, size_t n, const char *want, size_t want_len, int ci, size_t trunc)
{
    if (n != want_len && !(n == trunc && want_len > trunc))
        return 0;
    for (size_t i = 0; i < n; ++i) {
        if (ci ? lower(p[i]) != lower(want[i]) : p[i] != want[i])
            return 0;
    }
    return 1;
}

static void strip_brackets(const char **p, size_t *n)
{
    if (*n >= 2 && (*p)[0] == '[' && (*p)[*n - 1] == ']') {
        ++*p;
        *n -= 2;
    }
}

/* Does the label "user@host" in [p, p+n) name the target (or its alias)? */
static int label_is_target(const char *p, size_t n, const char *target, const char *alias)
{
    const char *at = NULL;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] == '@')
            at = p + i; /* last '@': host names can't contain one */
    }
    if (!at || !target || !*target)
        return 0;
    const char *tat = strrchr(target, '@');
    const char *tuser = target;
    size_t tuser_len = tat ? (size_t)(tat - target) : 0;
    const char *thost = tat ? tat + 1 : target;
    size_t thost_len = strlen(thost);
    strip_brackets(&thost, &thost_len);
    if (thost_len == 0)
        return 0;

    const char *user = p;
    size_t user_len = (size_t)(at - p);
    const char *host = at + 1;
    size_t host_len = n - user_len - 1;
    strip_brackets(&host, &host_len);
    if (host_len == 0)
        return 0;

    if (tuser_len > 0 && !label_matches(user, user_len, tuser, tuser_len, 0, SSH_USER_TRUNC))
        return 0;
    if (label_matches(host, host_len, thost, thost_len, 1, SSH_HOST_TRUNC))
        return 1;
    if (alias && *alias) {
        const char *a = alias;
        size_t alen = strlen(alias);
        strip_brackets(&a, &alen);
        return alen > 0 && label_matches(host, host_len, a, alen, 1, SSH_HOST_TRUNC);
    }
    return 0;
}

int zt_askpass_may_answer(const char *prompt, const char *target, const char *alias, int via_jump)
{
    if (!prompt || !contains_ci(prompt, "password"))
        return 0;

    /* "(user@host) ..." - keyboard-interactive, labelled by ssh (>= 8.4). */
    if (prompt[0] == '(') {
        const char *close = strchr(prompt, ')');
        if (close) {
            const char *p = prompt + 1;
            size_t n = (size_t)(close - p);
            if (memchr(p, '@', n))
                return label_is_target(p, n, target, alias);
        }
    }

    /* "user@host's password: " - password authentication, all ssh's own text. */
    static const char suffix[] = "'s password:";
    size_t len = strlen(prompt);
    while (len > 0 && (prompt[len - 1] == ' ' || prompt[len - 1] == '\t'))
        --len;
    size_t slen = sizeof suffix - 1;
    if (len > slen && memcmp(prompt + len - slen, suffix, slen) == 0) {
        size_t n = len - slen;
        if (memchr(prompt, '@', n) && !memchr(prompt, ' ', n))
            return label_is_target(prompt, n, target, alias);
    }

    /* Bare prompt: nobody vouches for where it came from. */
    return via_jump ? 0 : 1;
}
