// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CLAUDE_H
#define CLAUDE_H

#include "buf.h"
#include <sys/types.h>

typedef struct {
    pid_t  pid;           /* curl child process                    */
    int    pipe_fd;       /* read end of curl stdout               */
    char  *response;      /* accumulated response text (malloc'd)  */
    int    resp_len, resp_cap;
    char  *line_buf;      /* SSE line accumulator                  */
    int    line_len, line_cap;
    int    done;          /* 1 when stream finished                */
    int    error;         /* 1 on HTTP/parse error                 */
    char  *error_msg;     /* malloc'd error description            */
    /* Context sent with the request */
    char **context_files; /* file paths included as context        */
    int    context_count;
} ClaudeState;

ClaudeState *claude_state_new(void);
void         claude_state_free(ClaudeState *cs);

/* Send a message to the Claude API.  Returns 0 on success (fork+exec
   started), -1 on error.  The caller should poll cs->pipe_fd for data. */
int claude_send_message(ClaudeState *cs, const char *prompt,
                        const char *api_key, const char *model);

/* Read available SSE data from the pipe.  Call when poll() says pipe_fd
   is readable.  Returns 1 if new text was appended to cs->response,
   0 if nothing new, -1 on EOF/error (sets cs->done). */
int claude_read_stream(ClaudeState *cs);

/* Build a context string from the given file paths.  Caller must free(). */
char *claude_build_context(const char **paths, int count);

/* Extract fenced code blocks from a response.  Returns array of malloc'd
   strings (NULL-terminated).  Caller frees each string and the array. */
char **claude_extract_code_blocks(const char *response, int *out_count);

/* Rebuild chat buffer rows from the ClaudeState response text. */
void claude_update_buf(Buffer *buf, ClaudeState *cs, const char *prompt);

#endif
