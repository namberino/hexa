#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>


/*** defines ***/
#define CTRL_KEY(k) ((k) & 0x1f) // sets the upper 3 bits of the character to 0, which mirrors what the Ctrl key does in the terminal


/*** data ***/
struct termios orig_termios;


/*** function prototypes ***/
void enableRawMode();
void disableRawMode();
void die(const char* s);
void editorProcessKeypress();
void editorRefreshScreen();


/*** main ***/
int main()
{
	enableRawMode();	

	while (1)
	{
		editorRefreshScreen();
		editorProcessKeypress();
	}

	return 0;
}


/*** terminal ***/
void disableRawMode()
{
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
		die("tcsetattr");
}

void enableRawMode()
{
	if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
	atexit(disableRawMode); // called automatically when program exits

	struct termios raw = orig_termios;

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
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | INPCK | ISTRIP | IXON);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); 
	raw.c_cflag |= (CS8);
	raw.c_oflag &= ~(OPOST);
	raw.c_cc[VMIN] = 0; // sets the minimum number of bytes of input needed before read() can return
	raw.c_cc[VTIME] = 1; // sets the maximum amount of time to wait before read() returns

	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

// error handling (print out error if function returns -1)
void die(const char* s)
{
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3); // position the cursor

	perror(s);
	exit(1);
}

// wait for 1 keypress, then return it. deals with low-level terminal input
char editorReadKey()
{
	int nread;
	char c;

	while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
	{
		if (nread == -1 && errno != EAGAIN) die("read");
	}

	return c;
}


/*** input ***/
// wait for keypress, then handle it. deals with mapping keys to editor functions at a much higher level
void editorProcessKeypress()
{
	char c = editorReadKey();

	switch (c) 
	{
		case CTRL_KEY('q'):
			exit(0);
			break;
	}
}


/*** output ***/
// writing an escape sequence to the terminal
void editorRefreshScreen()
{
	write(STDOUT_FILENO, "\x1b[2J", 4);
	write(STDOUT_FILENO, "\x1b[H", 3); // position the cursor
}
