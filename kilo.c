/*** includes ***/
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <unistd.h>
#include <termios.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

/*** defines ***/
#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 3
#define CTRL_KEY(k) ((k) & 0x1f) // Ctrl + [A-Z] map to bytes 1-26

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** data ***/
typedef struct editorRow {
    int size;
    int rsize;
    char *chars;
    char *render;
} editorRow;

struct editorConfig {
    int cx;
    int cy;
    int rx;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    int numrows;
    editorRow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsgTime;
    struct termios origTermios;
};
struct editorConfig E_State;

/*** prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
char *editorPrompt(char *prompt);

/*** terminal ***/
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E_State.origTermios) == -1) die("tcsetattr");
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &E_State.origTermios) == -1) die("tcgetattr");
    atexit(disableRawMode);
    
    struct termios raw = E_State.origTermios;

    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey(void) {
    int nread;
    char c;

    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
        if (nread == 1 && errno != EAGAIN) die("read");

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'F': return END_KEY;
                    case 'H': return HOME_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'F': return END_KEY;
                case 'H': return HOME_KEY;
            }
        }

        return '\x1b';
    } else return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;

        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        
        return 0;
    }
}

/*** row operations ***/
int editorRowCxToRx(editorRow *row, int cx) {
    int rx = 0;
    for (int i = 0; i < cx; i++) {
        if (row->chars[i] == '\t') rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        rx++;
    }

    return rx;
}

void editorUpdateRow(editorRow *row) {
    int tabs = 0;
    for (int i = 0; i < row->size; i++)
        if (row->chars[i] == '\t') tabs++;
    
    free(row->render);
    row->render = malloc(row->size + tabs*(KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (int i = 0; i < row->size; i++) {
        if (row->chars[i] == '\t') {
            row->render[idx++] = ' ';
            while (idx % 8 != 0) row->render[idx++] = ' ';
        } else row->render[idx++] = row->chars[i];
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int pos, char *s, size_t len) {
    if (pos < 0 || pos > E_State.numrows) return;

    E_State.row = realloc(E_State.row, sizeof(editorRow) * (E_State.numrows + 1));
    memmove(&E_State.row[pos + 1], &E_State.row[pos], sizeof(editorRow) * (E_State.numrows - pos));

    E_State.row[pos].size = len;
    E_State.row[pos].chars = malloc(len + 1);
    memcpy(E_State.row[pos].chars, s, len);
    E_State.row[pos].chars[len] = '\0';

    E_State.row[pos].rsize = 0;
    E_State.row[pos].render = NULL;
    editorUpdateRow(&E_State.row[pos]);

    E_State.numrows++;
    E_State.dirty++;
}

void editorFreeRow(editorRow *row) {
    free(row->render);
    free(row->chars);
}

void editorDeleteRow(int pos) {
    if (pos < 0 || pos >= E_State.numrows) return;
    editorFreeRow(&E_State.row[pos]);
    memmove(&E_State.row[pos], &E_State.row[pos + 1], sizeof(editorRow) * (E_State.numrows - pos - 1));
    E_State.numrows--;
    E_State.dirty++;
}

void editorRowInsertChar(editorRow *row, int pos, int c) {
    if (pos < 0 || pos > row->size) pos = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[pos + 1], &row->chars[pos], row->size - pos + 1);
    row->size++;
    row->chars[pos] = c;
    editorUpdateRow(row);
}

void editorRowAppendString(editorRow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E_State.dirty++;
}

void editorRowDeleteChar(editorRow *row, int pos) {
    if (pos < 0 || pos >= row->size) return;
    memmove(&row->chars[pos], &row->chars[pos + 1], row->size - pos);
    row->size--;
    editorUpdateRow(row);
    E_State.dirty++;
}

/*** editor operations ***/
void editorInsertChar(int c) {
    if (E_State.cy == E_State.numrows) editorInsertRow(E_State.numrows, "", 0);
    editorRowInsertChar(&E_State.row[E_State.cy], E_State.cx, c);
    E_State.cx++;
}

void editorInsertNewLine(void) {
    if (E_State.cx == 0) editorInsertRow(E_State.cy, "", 0);
    else {
        editorRow *row = &E_State.row[E_State.cy];
        editorInsertRow(E_State.cy + 1, &row->chars[E_State.cx], row->size - E_State.cx);
        row = &E_State.row[E_State.cy];
        row->size = E_State.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E_State.cy++;
    E_State.cx = 0;
}

void editorDeleteChar(void) {
    if (E_State.cy == E_State.numrows) return;
    if (E_State.cx == 0 && E_State.cy == 0) return;

    editorRow *row = &E_State.row[E_State.cy];
    if (E_State.cx > 0) {
        editorRowDeleteChar(row, E_State.cx - 1);
        E_State.cx--;
    } else {
        E_State.cx = E_State.row[E_State.cy - 1].size;
        editorRowAppendString(&E_State.row[E_State.cy - 1], row->chars, row->size);
        editorDeleteRow(E_State.cy);
        E_State.cy--;
    }
}

/*** file i/o ***/
char *editorRowsToString(int *buflen) {
    int totlen = 0;
    for (int i = 0; i < E_State.numrows; i++)
        totlen += E_State.row[i].size + 1;
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (int i = 0; i < E_State.numrows; i++) {
        memcpy(p, E_State.row[i].chars, E_State.row[i].size);
        p += E_State.row[i].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename) {
    free(E_State.filename);
    E_State.filename = strdup(filename);
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) linelen--;
        editorInsertRow(E_State.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E_State.dirty = 0;
}

void editorSave(void) {
    if (E_State.filename == NULL){
        E_State.filename = editorPrompt("Save as: %s");
        if (E_State.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E_State.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E_State.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** append buffer ***/
struct AppendBuffer {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct AppendBuffer *appendBuffer, const char *s, int len) {
    char *new = realloc(appendBuffer->b, appendBuffer->len + len);

    if (new == NULL) return;
    memcpy(&new[appendBuffer->len], s, len);
    appendBuffer->b = new;
    appendBuffer->len += len;
}

void abFree(struct AppendBuffer *appendBuffer) {
    free(appendBuffer->b);
}

/*** output ***/
void editorScroll(void) {
    E_State.rx = 0;
    if (E_State.cy < E_State.numrows) E_State.rx = editorRowCxToRx(&E_State.row[E_State.cy], E_State.cx);
    if (E_State.cy < E_State.rowoff) E_State.rowoff = E_State.cy;
    if (E_State.cy >= E_State.rowoff + E_State.screenrows) E_State.rowoff = E_State.cy - E_State.screenrows + 1;
    if (E_State.rx < E_State.coloff) E_State.coloff = E_State.rx;
    if (E_State.rx >= E_State.coloff + E_State.screencols) E_State.coloff = E_State.rx - E_State.screencols + 1;
}

void editorDrawRows(struct AppendBuffer *appendBuffer) {
    int y;
    for (y = 0; y < E_State.screenrows; y++) {
        int filerow = y + E_State.rowoff;
        if (filerow >= E_State.numrows) {
            if (E_State.numrows == 0 && y == E_State.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Kilo editor -- version %s", KILO_VERSION);
                if (welcomelen > E_State.screencols) welcomelen = E_State.screencols;
                int padding = (E_State.screencols - welcomelen) / 2;
                if (padding) {
                    abAppend(appendBuffer, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(appendBuffer, " ", 1);
                abAppend(appendBuffer, welcome, welcomelen);
            } else abAppend(appendBuffer, "~", 1);
        } else {
            int len = E_State.row[filerow].rsize - E_State.coloff;
            if (len < 0) len = 0;
            if (len > E_State.screencols) len = E_State.screencols;
            abAppend(appendBuffer, &E_State.row[filerow].render[E_State.coloff], len);
        }

        abAppend(appendBuffer, "\x1b[K", 3);
        abAppend(appendBuffer, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct AppendBuffer *appendBuffer) {
    abAppend(appendBuffer, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                        E_State.filename ? E_State.filename : "[No Name]",
                        E_State.numrows,
                        E_State.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E_State.cy + 1, E_State.numrows);
    if (len > E_State.screencols) len = E_State.screencols;
    abAppend(appendBuffer, status, len);
    while (len < E_State.screencols) {
        if (E_State.screencols - len == rlen) {
            abAppend(appendBuffer, rstatus, rlen);
            break;
        } else {
            abAppend(appendBuffer, " ", 1);
            len++;
        }
    }
    abAppend(appendBuffer, "\x1b[m", 3);
    abAppend(appendBuffer, "\r\n", 2);
}

void editorDrawMessageBar(struct AppendBuffer *appendBuffer) {
    abAppend(appendBuffer, "\x1b[K", 3);
    int msglen = strlen(E_State.statusmsg);
    if (msglen > E_State.screencols) msglen = E_State.screencols;
    if (msglen && time(NULL) - E_State.statusmsgTime < 5) abAppend(appendBuffer, E_State.statusmsg, msglen);
}

void editorRefreshScreen(void) {
    editorScroll();

    struct AppendBuffer appendBuffer = ABUF_INIT;

    abAppend(&appendBuffer, "\x1b[?25l", 6);
    abAppend(&appendBuffer, "\x1b[H", 3);

    editorDrawRows(&appendBuffer);
    editorDrawStatusBar(&appendBuffer);
    editorDrawMessageBar(&appendBuffer);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E_State.cy - E_State.rowoff) + 1, (E_State.rx - E_State.coloff) + 1);
    abAppend(&appendBuffer, buf, strlen(buf));

    abAppend(&appendBuffer, "\x1b[?25h", 6);

    write(STDOUT_FILENO, appendBuffer.b, appendBuffer.len);
    abFree(&appendBuffer);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E_State.statusmsg, sizeof(E_State.statusmsg), fmt, ap);
    va_end(ap);
    E_State.statusmsgTime = time(NULL);
}

/*** input ***/
char *editorPrompt(char *prompt) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

void editorMoveCursor(int key) {
    editorRow *row = (E_State.cy >= E_State.numrows) ? NULL : &E_State.row[E_State.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E_State.cx != 0) E_State.cx--;
            else if (E_State.cy > 0) {
                E_State.cy--;
                E_State.cx = E_State.row[E_State.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E_State.cx < row->size) E_State.cx++;
            else if (row && E_State.cx == row->size) {
                E_State.cy++;
                E_State.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E_State.cy != 0) E_State.cy--;
            break;
        case ARROW_DOWN:
            if (E_State.cy < E_State.numrows) E_State.cy++;
            break;
    }

    row = (E_State.cy >= E_State.numrows) ? NULL : &E_State.row[E_State.cy];
    int rowlen = row ? row->size : 0;
    if (E_State.cx > rowlen)
        E_State.cx = rowlen;
}

void editorProcessKeypress(void) {
    static int quit_times = KILO_QUIT_TIMES;
    
    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewLine();
            break;

        case CTRL_KEY('q'):
            if (E_State.dirty > 0 && quit_times > 0) {
                editorSetStatusMessage("WARNING! File has unsaved changes. Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
            break;

        case HOME_KEY:
            E_State.cx = 0;
            break;

        case END_KEY:
            if (E_State.cy < E_State.numrows) E_State.cx = E_State.row[E_State.cy].size;
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDeleteChar();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP)
                    E_State.cy = E_State.rowoff;
                else if (c == PAGE_DOWN) {
                    E_State.cy = E_State.rowoff + E_State.screenrows - 1;
                    if (E_State.cy > E_State.numrows) E_State.cy = E_State.numrows;
                }
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;
        
        default:
            editorInsertChar(c);
            break;
    }

    quit_times = KILO_QUIT_TIMES;
}

/*** init ***/
void initEditor(void) {
    E_State.cx = 0;
    E_State.cy = 0;
    E_State.rx = 0;
    E_State.rowoff = 0;
    E_State.coloff = 0;
    E_State.numrows = 0;
    E_State.row = NULL;
    E_State.dirty = 0;
    E_State.filename = NULL;
    E_State.statusmsg[0] = '\0';
    E_State.statusmsgTime = 0;

    if (getWindowSize(&E_State.screenrows, &E_State.screencols) == -1) die("getWindowSize");
    E_State.screenrows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    
    if (argc >= 2) editorOpen(argv[1]);

    editorSetStatusMessage("HELP: Ctrl-Q = quit | Ctrl-S = save");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
