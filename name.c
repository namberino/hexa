#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f)
#define VERSION "0.0.1"
#define ABUF_INIT {NULL, 0}
#define TAB_STOP 8
#define QUIT_TIMES 1

// 1000: out of range of char so they don't conflict with normal keypress
enum editorKey 
{
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT = 1001,
    ARROW_UP = 1002,
    ARROW_DOWN = 1003,
    HOME_KEY = 1004,
    END_KEY = 1005,
    PAGE_UP = 1006,
    PAGE_DOWN = 1007,
    DEL_KEY = 1008
};


/*** data ***/
// editor row (for storage)
typedef struct erow 
{
    int size;
    int rsize; // size of contents of render
    char* chars;
    char* render;
} erow;

// append buffer
struct abuf 
{
    char* b;
    int len;
};
struct editorConfig 
{
    int cx, cy;
    int rx; // horizontal coordinate (index of render field as oppose to cx which is index of the chars field of erow)
    int rowoff; // vertical scroll offset
    int coloff; // horizontal scroll offset
    int screenrows;
    int screencols;
    int numrows;
    erow* row; // array for storing multiple lines
    int dirty; // keep track of whether the text loaded in the editor differs from what’s in the file
    char* filename;
    char statusmsg[80];
    time_t statusmsg_time; // contain the timestamp when we set a status message 
    struct termios orig_termios;
};

struct editorConfig E;


/*** function prototypes ***/
void editorSetStatusMessage(const char* fmt, ...);
void editorRefreshScreen();
char* editorPrompt(char* prompt);

/*** terminal ***/
// error handling (print out error if function returns -1)
void die(const char* s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3); // position the cursor

    perror(s);
    exit(1);
}

void disableRawMode() 
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() 
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode); // called automatically when program exits

    struct termios raw = E.orig_termios;

    // ICANON turns off canonical mode (allow reading input byte by byte instead of line by line)
	// ISIG turns off signals from Ctrl-C and Ctrl-Z
	// IXON turns off signals from Ctrl-Q and Ctrl-S
	// IEXTEN turns off signals from Ctrl-V and Ctrl-O
	// ICRNL turns off Ctrl-M (prevent tranlation of carriage 13 - '\r' into newlines 10 - '\n')
	// OPOST turns off the translation of '\n' to "\r\n" (carriage return)
	// When BRKINT is on, a break condition will cause a SIGINT signal to be sent to the program
	// INPCK enables parity checking
	// ISTRIP causes the 8th bit of each input byte to be stripped (set to 0)
	// CS8 is a bit mask with multiple bits (sets character size CS to 8 bits per byte)
	// Turn off ECHO to prevent letters being printed to terminal, similar to typing password when using sudo
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0; // sets the minimum number of bytes of input needed before read() can return
    raw.c_cc[VTIME] = 1; // sets the maximum amount of time to wait before read() returns

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// wait for 1 keypress, then return it. deals with low-level terminal input
int editorReadKey()
{
    int nread;
    char c;
    
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) 
    {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b')
    {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        
        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9') 
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';

                if (seq[2] == '~') 
                {
                    // reading the escape sequence
                    switch (seq[1]) 
                    {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    }
    else
    {
        return c;
    }
}

int getCursorPosition(int* rows, int* cols) 
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) 
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

// getting window size
int getWindowSize(int* rows, int* cols) 
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1; // fallback method for when ioctl doesn't work

        return getCursorPosition(rows, cols);
    } 
    else
    { 
        *cols = ws.ws_col;
        *rows = ws.ws_row;

        return 0;
    }
}

void abAppend(struct abuf *ab, const char *s, int len) 
{
    char* new = realloc(ab->b, ab->len + len);

    if (new == NULL) return;

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf* ab) 
{
    free(ab->b);
}

/*** row operations (no worries about where the cursor is) ***/
// converts a chars index into a render index (looping through all the characters to the left of cx, and figure out how many spaces each tab takes up)
// use rx % TAB_STOP to find out how many columns we are to the right of the last tab stop
// then subtract that from TAB_STOP - 1 to find out how many columns we are to the left of the next tab stop
// add that amount to rx to get just to the left of the next tab stop, and then the unconditional rx++ statement gets us right on the next tab stop
int editorRowCxToRx(erow* row, int cx) // basically a function for working with lines with tabs in them
{
    int rx = 0;
    int j;

    for (j = 0; j < cx; j++) 
    {
        if (row->chars[j] == '\t')
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        rx++;
    }

    return rx;
}

void editorUpdateRow(erow* row)
{
    int tabs = 0;
    int j;
    int idx = 0;

    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;
  
    free(row->render);
    row->render = malloc(row->size + tabs * (TAB_STOP - 1) + 1);
    
    for (j = 0; j < row->size; j++) 
    {
        if (row->chars[j] == '\t') 
        {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) row->render[idx++] = ' ';
        } 
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }
  
    row->render[idx] = '\0';
    row->rsize = idx;
}

// First validate 'at', then allocate memory for one more erow, and use memmove() to make room at the specified index for the new row.
void editorInsertRow(int at, char* s, size_t len)
{
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    // int at = E.numrows; // set to index of new row to initialize

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);

    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

// free memory owned by the erow being deleted 
/*
  First we validate the at index. 
  Then we free the memory owned by the row using editorFreeRow(). 
  We then use memmove() to overwrite the deleted row struct with the rest of the rows that come after it, and decrement numrows.
*/
void editorFreeRow(erow* row)
{
    free(row->render);
    free(row->chars);
}

void editorDelRow(int at) 
{
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));

    E.numrows--;
    E.dirty++;
}

// inserts a single character into an erow at a given position
void editorRowInsertChar(erow* row, int at, int c) 
{
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);

    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);

    row->size++;
    row->chars[at] = c;
    
    editorUpdateRow(row);
    E.dirty++;
}

// appends string to the end of a row
/*
  The row’s new size is row->size + len + 1 (including the null byte), so first we allocate that much memory for row->chars.
  Then we simply memcpy() the given string to the end of the contents of row->chars.
  We update row->size, call editorUpdateRow() as usual
*/
void editorRowAppendString(erow* row, char* s, size_t len)
{
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

// use memmove() to overwrite the deleted character with the characters that come after it
void editorRowDelChar(erow* row, int at)
{
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);

    row->size--;

    editorUpdateRow(row);
    E.dirty++;
}


/*** editor operations (no worries about details of modifying an erow) ***/
/*
  If E.cy == E.numrows, then the cursor is on the tilde line after the end of the file,
  so we need to append a new row to the file before inserting a character there.
  After inserting a character, we move the cursor forward so that the next character the user inserts will go after the character just inserted.
*/
void editorInsertChar(int c)
{
    if (E.cy == E.numrows)
        editorInsertRow(E.numrows, "", 0);

    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

/*
  If we’re at the beginning of a line, all we have to do is insert a new blank row before the line we’re on.
  Otherwise, we have to split the line we’re on into two rows.
  First we call editorInsertRow() and pass it the characters on the current row that are to the right of the cursor.
  That creates a new row after the current one, with the correct contents.
  Then we reassign the row pointer, because editorInsertRow() calls realloc(), which might move memory around on us and invalidate the pointer.
  Then we truncate the current row’s contents by setting its size to the position of the cursor, and we call editorUpdateRow() on the truncated row.

  In both cases, we increment E.cy, and set E.cx to 0 to move the cursor to the beginning of the row
*/
void editorInsertNewline()
{
    if (E.cx == 0)
    {
        editorInsertRow(E.cy, "", 0);
    } 
    else
    {
        erow* row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        row = &E.row[E.cy];
        row->size = E.cx;
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }

    E.cy++;
    E.cx = 0;
}

/*
  If the cursor’s past the end of the file, then there is nothing to delete, and we return
  Otherwise, we get the erow the cursor is on, and if there is a character to the left of the cursor,
  we delete it and move the cursor one to the left.
*/
void editorDelChar() 
{
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow* row = &E.row[E.cy];

    if (E.cx > 0) 
    {
        editorRowDelChar(row, E.cx - 1);
        E.cx--;
    }
    else
    {
        /*
          If the cursor is at the beginning of the first line, then there’s nothing to do, so we return immediately.
          Otherwise, if we find that E.cx == 0, we call editorRowAppendString() and then editorDelRow() as we planned.
          row points to the row we are deleting, so we append row->chars to the previous row, and then delete the row that E.cy is on.
          We set E.cx to the end of the contents of the previous row before appending to that row.
          That way, the cursor will end up at the point where the two lines joined
        */
        E.cx = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        editorDelRow(E.cy);
        E.cy--;
    }
}


/*** file IO ***/
/*
  First we add up the lengths of each row of text, adding 1 to each one for the newline character we’ll add to the end of each line.
  We save the total length into buflen, to tell the caller how long the string is.
  
  Then, after allocating the required memory, we loop through the rows, and memcpy() the contents of each row to the end of the buffer,
  appending a newline character after each row.

  We return buf, expecting the caller to free() the memory.
*/
char* editorRowsToString(int* buflen)
{
    int totlen = 0;
    int j;

    for (j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;

    *buflen = totlen;
    char* buf = malloc(totlen);
    char* p = buf;

    for (j = 0; j < E.numrows; j++)
    {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    
    return buf;
}

// for opening and reading file from disk
void editorOpen(char* filename)
{
    // get file name
    free(E.filename);
    E.filename = strdup(filename); // get copy of filename

    FILE* fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    // getline return -1 when it gets to the end of the file (as there's no more line to read)
    while ((linelen = getline(&line, &linecap, fp)) != -1)
    {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;

        editorInsertRow(E.numrows, line, linelen);
    }

    free(line);
    fclose(fp);
    E.dirty = 0;
}

/*
  New file: prompt for "Save as: "
  else: 
  Call editorRowsToString(), and write() the string to the path in E.filename.
  Tell open() we want to create a new file if it doesn’t already exist (O_CREAT), and we want to open it for reading and writing (O_RDWR).
  Because we used the O_CREAT flag, we have to pass an extra argument containing the mode (the permissions) the new file should have

  ftruncate() sets the file’s size to the specified length. 
  If the file is larger than that, it will cut off any data at the end of the file to make it that length. 
  If the file is shorter, it will add 0 bytes at the end to make it that length.

  open() and ftruncate() both return -1 on error.
  We expect write() to return the number of bytes we told it to write. 
  Whether or not an error occurred, we ensure that the file is closed and the memory that buf points to is freed.
*/
void editorSave()
{
    if (E.filename == NULL)
    {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)");

        if (E.filename == NULL) 
        {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char* buf = editorRowsToString(&len);
    int fd = open(E.filename, O_RDWR | O_CREAT, 0644); // 0644: the standard permissions for text file
    
    if (fd != -1) 
    {
        if (ftruncate(fd, len) != -1) 
        {
            if (write(fd, buf, len) == len) 
            {
                close(fd);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        
        close(fd);
    }

    free(buf);
    editorSetStatusMessage("Save failed! I/O error: %s", strerror(errno));
}


/*** output ***/
// check if cursor moved outside of screen, if so, adjust E.rowoff so that cursor is inside visible window
void editorScroll() 
{
    E.rx = E.cx;

    if (E.cy < E.numrows)
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);

    // check if cursor is above the visible window
    if (E.cy < E.rowoff)
        E.rowoff = E.cy;

    // check if cursor is past the bottom of the visible window
    if (E.cy >= E.rowoff + E.screenrows)
        E.rowoff = E.cy - E.screenrows + 1; // since E.rowoff refers to top of the screen

    // check if cursor is to the left of the visible window
    if (E.rx < E.coloff)
        E.coloff = E.rx;

    // check if cursor is to the right of the visible window
    if (E.rx >= E.coloff + E.screencols)
        E.coloff = E.rx - E.screencols + 1;
}

// handle drawing each row of buffer of text being edited
void editorDrawRows(struct abuf* ab)
{
    int y;

    for (y = 0; y < E.screenrows; y++) 
    {
        int filerow = y + E.rowoff; // for displaying the row of the file at y position

        if (filerow >= E.numrows)
        {
            if (E.numrows == 0 && y == E.screenrows / 3)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "Unnamed Editor - Version %s", VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;

                int padding = (E.screencols - welcomelen) / 2;

                if (padding) 
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);

                abAppend(ab, welcome, welcomelen);
            }
            else 
            {
                abAppend(ab, "~", 1);
            }
        }
        else 
        {
            // subtract the number of characters that are to the left of the offset from the length of the row
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloff], len); // display characters in 'render'
        }

        abAppend(ab, "\x1b[K", 3);

        abAppend(ab, "\r\n", 2);
    }
}

// escape sequence '[7m' switches to inverted colors, '[m' switches back to normal formatting
/*
  The current line is stored in E.cy, which we add 1 to since E.cy is 0-indexed.

  After printing the first status string, we want to keep printing spaces until we get to the point where 
  if we printed the second status string, it would end up against the right edge of the screen.

  That happens when E.screencols - len is equal to the length of the second status string. 
  At that point we print the status string and break out of the loop, as the entire status bar has now been printed.
*/
void editorDrawStatusBar(struct abuf* ab)
{
    abAppend(ab, "\x1b[7m", 4);

    // state of E.dirty is (modified) in status bar
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);

    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);

    while (len < E.screencols)
    {
        if (E.screencols - len == rlen)
        {
            abAppend(ab, rstatus, rlen);
            break;
        }
        else
        {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2); // new line (for second status bar)
}

/*
  Clear message bar with '[K' escape sequence
  Make sure the message will fit the width of the screen, then display the message (only if the message is less than 5 seconds old)
*/
void editorDrawMessageBar(struct abuf* ab) 
{
    abAppend(ab, "\x1b[K", 3);

    int msglen = strlen(E.statusmsg);

    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

// writing an escape sequence to the terminal
void editorRefreshScreen() 
{
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1); // reposition the cursor by subtracting rowoff with cy and coloff with cx
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*
  Store the resulting string in E.statusmsg, and set E.statusmsg_time to the current time, which can be gotten by passing NULL to time()
  We pass fmt and ap to vsnprintf() and it takes care of reading the format string and calling va_arg() to get each argument
*/
void editorSetStatusMessage(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap); // help us make our own printf()-style function
    va_end(ap);

    E.statusmsg_time = time(NULL);
}

/*** input ***/
/*
  The user’s input is stored in buf, which is a dynamically allocated string (empty).
  We enter an infinite loop that repeatedly sets the status message, refreshes the screen, and waits for a keypress to handle.
  The prompt is expected to be a format string containing a %s, which is where the user’s input will be displayed.

  When the user presses Enter, and their input is not empty, the status message is cleared and their input is returned.
  Otherwise, when they input a printable character, we append it to buf.
  If buflen has reached the maximum capacity we allocated (stored in bufsize),
  then we double bufsize and allocate that amount of memory before appending to buf.
  We also make sure that buf ends with a \0 character,
  because both editorSetStatusMessage() and the caller of editorPrompt() will use it to know where the string ends.
*/
char* editorPrompt(char* prompt)
{
    size_t bufsize = 128;
    char* buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';

    while (1)
    {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();

        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) // allow backspace in prompt
        {
            if (buflen != 0) buf[--buflen] = '\0';
        }
        else if (c == '\x1b') // When an input prompt is cancelled, we free() the buf ourselves and return NULL
        {
            editorSetStatusMessage("");
            free(buf);
            return NULL;
        }
        else if (c == '\r')
        {
            if (buflen != 0)
            {
                editorSetStatusMessage("");
                return buf;
            }
        } 
        else if (!iscntrl(c) && c < 128)
        {
            if (buflen == bufsize - 1)
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }

            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
    }
}

void editorMoveCursor(int key)
{
    // check if cursor is on the line
    // If it is, then the row variable will point to the erow that the cursor is on, 
    // and we’ll check whether E.cx is to the left of the end of that line before we allow the cursor to move to the right
    erow* row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy]; // for limiting scrolling past the end of the current line

    switch (key) 
    {
        case ARROW_LEFT:
            if (E.cx != 0)
            {
                E.cx--;
            }
            else if (E.cy > 0) 
            {
                // move cursor up a line if left arrow is pressed at the beginning of a line (E.cx == 0)
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) // limiting scrolling past the end of the current line
            {
                E.cx++;
            }
            else if (row && E.cx == row->size) // if there is a row and E.cx is the row size (at the end of the line)
            {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0)
                E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows)
                E.cy++;
            break;
    }

    // set row again, since E.cy could point to a different line than it did before
    // set E.cx to the end of that line if E.cx is to the right of the end of that line
    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;

    if (E.cx > rowlen)
        E.cx = rowlen;
}

// wait for keypress, then handle it. deals with mapping keys to editor functions at a much higher level
void editorProcessKeypress() 
{
    // We use a static variable in editorProcessKeypress() to keep track of how many more times the user must press Ctrl-Q to quit
    static int quit_times = QUIT_TIMES;

    int c = editorReadKey();

    switch (c) 
    {
        case '\r': // enter key
            editorInsertNewline();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;

        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0)
            {
                editorSetStatusMessage("WARNING! File has unsaved changes. Press Ctrl-Q again to quit.");
                quit_times--; // When quit_times hits 0, the program to exits

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
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cy < E.numrows)
                E.cx = E.row[E.cy].size;
            break;

        // move to either the beginning or the end of the screen
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (c == PAGE_UP)
                {
                    E.cy = E.rowoff;
                } 
                else if (c == PAGE_DOWN)
                {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }

                int times = E.screenrows; // how many times to move

                while (times--)
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b': // escape key
            break;

        default:
            editorInsertChar(c);
            break;
    }

    quit_times = QUIT_TIMES;
}

/*** main ***/
void initEditor() 
{
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.dirty = 0;
    E.row = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2; // so that editorDrawRows() doesn’t try to draw a line of text at the bottom of the screen (- 2 for 2 rows)
}

int main(int argc, char* argv[]) 
{
    enableRawMode();
    initEditor();
    if (argc >= 2)
        editorOpen(argv[1]);

    editorSetStatusMessage("Help: Ctrl-S = save | Ctrl-Q = quit");

    while (1) 
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
