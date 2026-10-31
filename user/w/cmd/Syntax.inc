/* Shell metacharacters: redirection and concurrent anonymous-pipe pipelines. */
#define CMD_MAX_PIPELINE 8

typedef struct _CMD_REDIRECTION {
    HANDLE Input;
    HANDLE Output;
    HANDLE Error;
    HANDLE Owned[3];
    unsigned OwnedCount;
} CMD_REDIRECTION;

static HANDLE open_redirection(const char *name, int input, int append)
{
    char path[260];
    if (!normalize_path(name, path)) return INVALID_HANDLE_VALUE;
    HANDLE file = CreateFileA(path, input ? 0x80000000u : 0x40000000u,
                              input ? 1 : 0, 0, input ? 3 : (append ? 4 : 2),
                              0, 0);
    if (file != INVALID_HANDLE_VALUE && append)
        SetFilePointer(file, 0, 0, 2);
    return file;
}

static int parse_redirections(char *line, HANDLE input, HANDLE output,
                              HANDLE error, CMD_REDIRECTION *redirection)
{
    redirection->Input = input;
    redirection->Output = output;
    redirection->Error = error;
    redirection->OwnedCount = 0;
    char cleaned[CMD_LINE_MAX];
    unsigned in = 0, out = 0;
    int quoted = 0;
    while (line[in] && out + 1 < sizeof(cleaned)) {
        if (line[in] == '"') {
            quoted = !quoted;
            cleaned[out++] = line[in++];
            continue;
        }
        int stream = 1;
        if (!quoted && line[in] == '2' && line[in + 1] == '>') {
            stream = 2; in++;
        }
        if (!quoted && (line[in] == '>' || line[in] == '<')) {
            int input_redirect = line[in] == '<';
            int append = !input_redirect && line[in + 1] == '>';
            in += append ? 2 : 1;
            while (line[in] == ' ' || line[in] == '\t') in++;
            char name[260]; unsigned n = 0;
            int name_quoted = line[in] == '"';
            if (name_quoted) in++;
            while (line[in] && n + 1 < sizeof(name)) {
                if ((name_quoted && line[in] == '"') ||
                    (!name_quoted && (line[in] == ' ' || line[in] == '\t' ||
                                      line[in] == '>' || line[in] == '<')))
                    break;
                name[n++] = line[in++];
            }
            if (name_quoted && line[in] == '"') in++;
            name[n] = 0;
            HANDLE handle = open_redirection(name, input_redirect, append);
            if (handle == INVALID_HANDLE_VALUE) {
                write_s("The redirection target could not be opened.\r\n");
                goto failed;
            }
            HANDLE *target = input_redirect ? &redirection->Input :
                             stream == 2 ? &redirection->Error :
                                           &redirection->Output;
            unsigned slot = 0;
            while (slot < redirection->OwnedCount &&
                   redirection->Owned[slot] != *target) slot++;
            if (slot < redirection->OwnedCount) {
                CloseHandle(redirection->Owned[slot]);
            } else if (redirection->OwnedCount < 3) {
                redirection->OwnedCount++;
            } else {
                CloseHandle(handle);
                goto failed;
            }
            redirection->Owned[slot] = handle;
            *target = handle;
            continue;
        }
        cleaned[out++] = line[in++];
    }
    cleaned[out] = 0;
    trim_right(cleaned);
    out = 0;
    while ((line[out] = cleaned[out])) out++;
    return 1;
failed:
    for (unsigned i = 0; i < redirection->OwnedCount; i++)
        CloseHandle(redirection->Owned[i]);
    redirection->OwnedCount = 0;
    g_errorlevel = 1;
    return 0;
}

static void close_redirections(CMD_REDIRECTION *redirection)
{
    for (unsigned i = 0; i < redirection->OwnedCount; i++)
        CloseHandle(redirection->Owned[i]);
}

static int execute_pipeline(char *line)
{
    char *segments[CMD_MAX_PIPELINE];
    unsigned count = 1;
    segments[0] = line;
    int quoted = 0;
    for (unsigned i = 0; line[i]; i++) {
        if (line[i] == '"') quoted = !quoted;
        else if (!quoted && line[i] == '^' && line[i + 1]) i++;
        else if (!quoted && line[i] == '|' && count < CMD_MAX_PIPELINE) {
            line[i] = 0;
            segments[count++] = &line[i + 1];
        }
    }

    PROCESS_INFORMATION processes[CMD_MAX_PIPELINE];
    unsigned process_count = 0;
    HANDLE previous_read = 0;
    int keep_running = 1;
    for (unsigned i = 0; i < count; i++) {
        HANDLE pipe_read = 0, pipe_write = 0;
        if (i + 1 < count && !CreatePipe(&pipe_read, &pipe_write, 0, 0)) {
            write_s("Unable to create pipe.\r\n");
            g_errorlevel = 1;
            keep_running = 1;
            break;
        }
        CMD_REDIRECTION redirection;
        HANDLE stage_input = previous_read ? previous_read : g_in;
        HANDLE stage_output = pipe_write ? pipe_write : g_out;
        if (!parse_redirections(segments[i], stage_input, stage_output, g_out,
                                &redirection)) {
            if (pipe_read) CloseHandle(pipe_read);
            if (pipe_write) CloseHandle(pipe_write);
            break;
        }

        HANDLE saved_in = g_in, saved_out = g_out, saved_err = g_err;
        g_in = redirection.Input;
        g_out = redirection.Output;
        g_err = redirection.Error;
        g_async_launch = count > 1 || g_background_launch;
        g_launched_process.hProcess = 0;
        keep_running = dispatch(segments[i]);
        if (g_async_launch && g_launched_process.hProcess)
            processes[process_count++] = g_launched_process;
        g_async_launch = 0;
        g_in = saved_in; g_out = saved_out; g_err = saved_err;
        close_redirections(&redirection);
        if (previous_read) CloseHandle(previous_read);
        if (pipe_write) CloseHandle(pipe_write);
        previous_read = pipe_read;
        if (!keep_running) break;
    }
    if (previous_read) CloseHandle(previous_read);
    for (unsigned i = 0; i < process_count; i++) {
        if (!g_background_launch) {
            WaitForSingleObject(processes[i].hProcess, (DWORD)-1);
            if (i + 1 == process_count)
                GetExitCodeProcess(processes[i].hProcess, &g_errorlevel);
        }
        if (processes[i].hProcess) CloseHandle(processes[i].hProcess);
        if (processes[i].hThread) CloseHandle(processes[i].hThread);
    }
    return keep_running;
}

/* Parse shell control operators outside quotes. A single '&' detaches the
 * preceding command; '&&' and '||' are conditional, ';' is unconditional. */
static int execute_line(char *line)
{
    char *start = line;
    int quoted = 0, run_next = 1, keep_running = 1;
    for (unsigned i = 0; ; i++) {
        char c = line[i];
        if (c == '"') quoted = !quoted;
        if (!c || (!quoted && c == '^' && line[i + 1])) { if (c) { i++; continue; } }
        if (!c || (!quoted && (c == ';' || c == '&' ||
            (c == '|' && line[i + 1] == '|')))) {
            char op = c;
            unsigned op_len = 0;
            if (c) {
                op_len = 1;
                if ((c == '&' || c == '|') && line[i + 1] == c) op_len = 2;
                line[i] = 0;
            }
            char *command = skip_spaces(start);
            trim_right(command);
            if (*command && run_next) {
                int background = op == '&' && op_len == 1;
                g_background_launch = background;
                keep_running = execute_pipeline(command);
                g_background_launch = 0;
            }
            if (!keep_running) return 0;
            if (!c) break;
            if (op == '&' && op_len == 2) run_next = (g_errorlevel == 0);
            else if (op == '|' && op_len == 2) run_next = (g_errorlevel != 0);
            else run_next = 1;
            i += op_len - 1;
            start = &line[i + 1];
        }
        if (!c) break;
    }
    return keep_running;
}
