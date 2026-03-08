// SPDX-License-Identifier: GPL-3.0-or-later
#include "claude.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Lifecycle ───────────────────────────────────────────────────────── */

ClaudeState *claude_state_new(void) {
    ClaudeState *cs = calloc(1, sizeof(ClaudeState));
    if (!cs) return NULL;
    cs->pipe_fd = -1;
    cs->pid     = -1;
    return cs;
}

void claude_state_free(ClaudeState *cs) {
    if (!cs) return;
    if (cs->pipe_fd >= 0) close(cs->pipe_fd);
    if (cs->pid > 0) {
        kill(cs->pid, SIGTERM);
        waitpid(cs->pid, NULL, WNOHANG);
    }
    free(cs->response);
    free(cs->line_buf);
    free(cs->error_msg);
    for (int i = 0; i < cs->context_count; i++)
        free(cs->context_files[i]);
    free(cs->context_files);
    free(cs);
}

/* ── JSON escaping ───────────────────────────────────────────────────── */

/* Escape a string for embedding in a JSON value.  Caller must free(). */
static char *json_escape(const char *s) {
    int len = (int)strlen(s);
    /* Worst case: every char becomes \uXXXX (6 bytes). */
    char *out = malloc(len * 6 + 1);
    if (!out) return NULL;
    int o = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        case '\b': out[o++] = '\\'; out[o++] = 'b';  break;
        case '\f': out[o++] = '\\'; out[o++] = 'f';  break;
        default:
            if (c < 0x20) {
                o += sprintf(out + o, "\\u%04x", c);
            } else {
                out[o++] = c;
            }
        }
    }
    out[o] = '\0';
    return out;
}

/* ── Send message via fork+exec curl ─────────────────────────────────── */

int claude_send_message(ClaudeState *cs, const char *prompt,
                        const char *api_key, const char *model) {
    if (!api_key || !api_key[0]) {
        cs->error = 1;
        cs->error_msg = strdup("No API key (set via qe.set_option(\"claude_api_key\", ...))");
        cs->done = 1;
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) < 0) {
        cs->error = 1;
        cs->error_msg = strdup("pipe() failed");
        cs->done = 1;
        return -1;
    }

    /* Build JSON body. */
    char *esc_prompt = json_escape(prompt);
    if (!esc_prompt) {
        close(pipefd[0]); close(pipefd[1]);
        cs->error = 1; cs->error_msg = strdup("OOM"); cs->done = 1;
        return -1;
    }

    /* Build context from any attached files. */
    char *context_block = NULL;
    if (cs->context_count > 0) {
        context_block = claude_build_context(
            (const char **)cs->context_files, cs->context_count);
    }

    /* Build system prompt with context. */
    char *sys_text = NULL;
    if (context_block) {
        char *esc_ctx = json_escape(context_block);
        int slen = (int)strlen(esc_ctx) + 256;
        sys_text = malloc(slen);
        snprintf(sys_text, slen,
                 "You are a helpful coding assistant inside a terminal text editor called qe. "
                 "Be concise. When suggesting code changes, use fenced code blocks with the language.\\n\\n"
                 "Context files:\\n%s", esc_ctx);
        free(esc_ctx);
        free(context_block);
    } else {
        sys_text = strdup(
            "You are a helpful coding assistant inside a terminal text editor called qe. "
            "Be concise. When suggesting code changes, use fenced code blocks with the language.");
    }

    /* Build the full JSON body. */
    int body_cap = (int)strlen(esc_prompt) + (int)strlen(sys_text)
                 + (int)strlen(model) + 512;
    char *body = malloc(body_cap);
    snprintf(body, body_cap,
             "{\"model\":\"%s\",\"max_tokens\":4096,\"stream\":true,"
             "\"system\":\"%s\","
             "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
             model, sys_text, esc_prompt);
    free(esc_prompt);
    free(sys_text);

    /* Build auth header. */
    char auth[300];
    snprintf(auth, sizeof(auth), "x-api-key: %s", api_key);

    pid_t pid = fork();
    if (pid < 0) {
        free(body);
        close(pipefd[0]); close(pipefd[1]);
        cs->error = 1; cs->error_msg = strdup("fork() failed"); cs->done = 1;
        return -1;
    }

    if (pid == 0) {
        /* Child: redirect stdout to pipe, exec curl. */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        /* Suppress stderr from curl. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }

        execlp("curl", "curl", "-s", "-N",
               "-X", "POST",
               "-H", "Content-Type: application/json",
               "-H", auth,
               "-H", "anthropic-version: 2023-06-01",
               "-d", body,
               "https://api.anthropic.com/v1/messages",
               (char *)NULL);
        _exit(127);
    }

    /* Parent. */
    free(body);
    close(pipefd[1]);

    /* Make pipe non-blocking. */
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    cs->pid     = pid;
    cs->pipe_fd = pipefd[0];
    cs->done    = 0;
    cs->error   = 0;
    return 0;
}

/* ── SSE line parser ─────────────────────────────────────────────────── */

/* Append text to the response buffer. */
static void resp_append(ClaudeState *cs, const char *s, int len) {
    if (cs->resp_len + len + 1 > cs->resp_cap) {
        int newcap = (cs->resp_cap ? cs->resp_cap * 2 : 1024);
        if (newcap < cs->resp_len + len + 1) newcap = cs->resp_len + len + 1;
        cs->response = realloc(cs->response, newcap);
        cs->resp_cap = newcap;
    }
    memcpy(cs->response + cs->resp_len, s, len);
    cs->resp_len += len;
    cs->response[cs->resp_len] = '\0';
}

/* Extract text delta from an SSE data line containing a content_block_delta event.
   Very simple: look for "text":" and extract until the closing quote.
   Returns malloc'd string or NULL. */
static char *extract_text_delta(const char *json) {
    /* Only process content_block_delta events. */
    if (!strstr(json, "content_block_delta")) return NULL;

    const char *key = "\"text\":\"";
    const char *p = strstr(json, key);
    if (!p) return NULL;
    p += strlen(key);

    /* Scan to closing quote, handling escapes. */
    int cap = 256;
    char *out = malloc(cap);
    int o = 0;
    while (*p && *p != '"') {
        if (o + 6 >= cap) { cap *= 2; out = realloc(out, cap); }
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n':  out[o++] = '\n'; break;
            case 'r':  out[o++] = '\r'; break;
            case 't':  out[o++] = '\t'; break;
            case '"':  out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; break;
            case '/':  out[o++] = '/';  break;
            case 'u': {
                /* Parse \uXXXX — basic ASCII only. */
                unsigned int cp = 0;
                for (int i = 0; i < 4 && p[1]; i++) {
                    p++;
                    char c = *p;
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= c - '0';
                    else if (c >= 'a' && c <= 'f') cp |= 10 + c - 'a';
                    else if (c >= 'A' && c <= 'F') cp |= 10 + c - 'A';
                }
                if (cp < 128) out[o++] = (char)cp;
                else out[o++] = '?';
                break;
            }
            default: out[o++] = *p; break;
            }
        } else {
            out[o++] = *p;
        }
        p++;
    }
    out[o] = '\0';
    if (o == 0) { free(out); return NULL; }
    return out;
}

/* Check if an SSE data line contains an error event. */
static char *extract_error(const char *json) {
    if (!strstr(json, "\"error\"")) return NULL;
    const char *key = "\"message\":\"";
    const char *p = strstr(json, key);
    if (!p) return strdup("API error (unknown)");
    p += strlen(key);
    const char *end = strchr(p, '"');
    if (!end) return strdup("API error (parse failed)");
    int len = (int)(end - p);
    char *msg = malloc(len + 1);
    memcpy(msg, p, len);
    msg[len] = '\0';
    return msg;
}

int claude_read_stream(ClaudeState *cs) {
    if (cs->done || cs->pipe_fd < 0) return -1;

    char buf[4096];
    int nr = read(cs->pipe_fd, buf, sizeof(buf) - 1);
    if (nr < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        cs->done = 1;
        return -1;
    }
    if (nr == 0) {
        /* EOF — curl exited. */
        cs->done = 1;
        close(cs->pipe_fd);
        cs->pipe_fd = -1;
        waitpid(cs->pid, NULL, 0);
        cs->pid = -1;
        return -1;
    }

    int got_text = 0;

    /* Feed bytes into line_buf, process complete lines. */
    for (int i = 0; i < nr; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\r') {
            if (cs->line_len > 0) {
                /* Null-terminate the line. */
                if (cs->line_len >= cs->line_cap) {
                    cs->line_cap = cs->line_len + 1;
                    cs->line_buf = realloc(cs->line_buf, cs->line_cap);
                }
                cs->line_buf[cs->line_len] = '\0';

                /* Process SSE "data: " lines. */
                if (strncmp(cs->line_buf, "data: ", 6) == 0) {
                    const char *json = cs->line_buf + 6;

                    /* Check for errors first. */
                    char *err = extract_error(json);
                    if (err) {
                        cs->error = 1;
                        free(cs->error_msg);
                        cs->error_msg = err;
                    }

                    /* Check for stream end. */
                    if (strstr(json, "message_stop")) {
                        cs->done = 1;
                    }

                    /* Extract text deltas. */
                    char *text = extract_text_delta(json);
                    if (text) {
                        resp_append(cs, text, (int)strlen(text));
                        free(text);
                        got_text = 1;
                    }
                }
                cs->line_len = 0;
            }
        } else {
            /* Append char to line_buf. */
            if (cs->line_len + 1 >= cs->line_cap) {
                cs->line_cap = cs->line_cap ? cs->line_cap * 2 : 256;
                cs->line_buf = realloc(cs->line_buf, cs->line_cap);
            }
            cs->line_buf[cs->line_len++] = c;
        }
    }

    return got_text ? 1 : 0;
}

/* ── Buffer update ───────────────────────────────────────────────────── */

void claude_update_buf(Buffer *buf, ClaudeState *cs, const char *prompt) {
    /* Clear existing rows. */
    for (int i = 0; i < buf->numrows; i++) {
        free(buf->rows[i].chars);
        free(buf->rows[i].hl);
    }
    free(buf->rows);
    buf->rows    = NULL;
    buf->numrows = 0;

    /* Insert user prompt lines. */
    char hdr[512];
    snprintf(hdr, sizeof(hdr), "You: %s", prompt);
    buf_insert_row(buf, buf->numrows, hdr, (int)strlen(hdr));
    buf_insert_row(buf, buf->numrows, "", 0);

    /* Insert response (or status). */
    if (cs->error && cs->error_msg) {
        char err[512];
        snprintf(err, sizeof(err), "[Error] %s", cs->error_msg);
        buf_insert_row(buf, buf->numrows, err, (int)strlen(err));
    } else if (cs->response && cs->resp_len > 0) {
        /* Split response into lines. */
        const char *p = cs->response;
        while (*p) {
            const char *nl = strchr(p, '\n');
            int len = nl ? (int)(nl - p) : (int)strlen(p);
            buf_insert_row(buf, buf->numrows, p, len);
            p += len;
            if (nl) p++;
        }
    } else if (!cs->done) {
        const char *wait = "Waiting for response...";
        buf_insert_row(buf, buf->numrows, wait, (int)strlen(wait));
    }

    if (cs->done && !cs->error) {
        buf_insert_row(buf, buf->numrows, "", 0);
        const char *fin = "[Done]";
        buf_insert_row(buf, buf->numrows, fin, (int)strlen(fin));
    }

    buf->dirty = 0;
}

/* ── Context builder ─────────────────────────────────────────────────── */

char *claude_build_context(const char **paths, int count) {
    int cap = 4096, len = 0;
    char *out = malloc(cap);
    out[0] = '\0';

    for (int i = 0; i < count; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;

        /* Read file contents (limit to 64KB per file). */
        char content[65536];
        int clen = (int)fread(content, 1, sizeof(content) - 1, f);
        fclose(f);
        content[clen] = '\0';

        int needed = clen + (int)strlen(paths[i]) + 64;
        while (len + needed >= cap) { cap *= 2; out = realloc(out, cap); }

        len += snprintf(out + len, cap - len,
                        "<file path=\"%s\">\n%s\n</file>\n", paths[i], content);
    }

    return out;
}

/* ── Code block extractor ────────────────────────────────────────────── */

char **claude_extract_code_blocks(const char *response, int *out_count) {
    *out_count = 0;
    if (!response) return NULL;

    int cap = 4, cnt = 0;
    char **blocks = malloc(sizeof(char *) * (cap + 1));

    const char *p = response;
    while ((p = strstr(p, "```")) != NULL) {
        p += 3;
        /* Skip language tag on the opening fence. */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;

        /* Find closing fence. */
        const char *end = strstr(p, "```");
        if (!end) break;

        int blen = (int)(end - p);
        /* Strip trailing newline if present. */
        if (blen > 0 && p[blen - 1] == '\n') blen--;

        if (cnt >= cap) {
            cap *= 2;
            blocks = realloc(blocks, sizeof(char *) * (cap + 1));
        }
        blocks[cnt] = malloc(blen + 1);
        memcpy(blocks[cnt], p, blen);
        blocks[cnt][blen] = '\0';
        cnt++;

        p = end + 3;
    }

    blocks[cnt] = NULL;
    *out_count = cnt;
    return blocks;
}
