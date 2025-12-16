# MyTerm - Custom X11 Termina

## Overview

**MyTerm** is a custom graphical terminal emulator built entirely in C using **X11, POSIX threads**, and **semaphores**.
It replicates and enhances the behavior of a traditional Unix shell while providing a modern GUI interface with multiple tabs, real-time output, and advanced input handling.
The terminal supports Unicode and multiline input, persistent command history, file and command autocomplete, background/foreground process control, and a unique multiWatch feature for parallel command execution.
With keyboard navigation, signal handling (Ctrl+C, Ctrl+Z, Ctrl+R), and persistent history across sessions, MyTerm delivers both the power of Unix and the convenience of a graphical shell.

---

## Dependencies and Installation

### 1. Install Required Packages

* GCC Compiler
* X11 and Xext development libraries (libx11-dev, libxext-dev)

To install dependencies (on Ubuntu/Debian):

```bash
sudo apt-get install libx11-dev libxext-dev
```

### 2. Build the Project

```bash
cd myterm_project
make clean
make
```

### 3. Run the Terminal

```bash
./myterm
```

This will open the custom graphical terminal window.

---

## Features and Usage
### 1. Graphical User Interface

Implemented a custom GUI using **X11** that mimics a bash terminal with multiple independent tabs, each running its own shell instance.

**Open, switch, and close tabs**

* **Press F1** → Open a new tab
* **Press F2** → Switch between tabs
* **Press F3** → Close the active tab

---

### 2. Run External Commands

Implemented execution of external programs using `fork()` and `execvp()` system calls — allowing standard Linux commands to run directly inside the terminal.

**Example commands:**

```bash
cd ~/Downloads/mydir  
ls -l  
gcc -o myprog myprog.c  
./myprog
```

---

### 3. Multiline and Unicode Input

Supports **multiline and unicode input/output** using `read()`, `write()`, and `setlocale()`, enabling multilingual text and newline support.

**Example command:**

```bash
echo "We say Hello in English \n\
Aloha mākou ma ka ʻōlelo Hawaiʻi"
```

**Output:**

```
We say Hello in English  
Aloha mākou ma ka ʻōlelo Hawaiʻi
```

---

### 4. Input Redirection (`<`)

Implemented **input redirection** using `dup2()`, allowing commands to take input directly from files instead of the keyboard.

**Example commands:**

```bash
./a.out < infile.txt  
sort < somefile.txt
```

---

### 5. Output Redirection (`>`)

Implemented **output redirection** using `dup2()`, allowing commands to write their output to files instead of displaying it on the terminal.

**Example commands:**

```bash
./a.out > outfile.txt  
ls > abc
```

**Combination of Input and Output Redirection:**

```bash
./a.out < infile.txt > outfile.txt
```

---

### 6. Pipe Support (`|`)

Implemented **pipe chaining** using the `pipe()` system call — the standard output of one command is redirected as the standard input to the next command.

**Example commands:**

```bash
ls *.txt | wc -l  
cat abc.c | sort | more  
ls *.txt | xargs rm
```

---

### 7. MultiWatch Command

Implemented a custom **`multiWatch`** command that runs multiple commands **in parallel** and continuously displays their outputs with **timestamps** and **command names**.

**Usage:**

```bash
multiWatch ["echo Hello", "ping google.com", "date"]
```

**Behavior:**

* Runs each command in a separate process.
* Continuously displays their outputs along with timestamps.
* Temporary files `.temp.PID.txt` are created to store process output.
* Stops all processes on pressing **Ctrl+C** and cleans up temp files.

---

### 8. Line Navigation with Ctrl+A and Ctrl+E

Added **bash-like cursor navigation** support inside the command line.

**Key bindings:**

* **Ctrl+A** → Move cursor to the start of the line.
* **Ctrl+E** → Move cursor to the end of the line.

---

### 9. Command Interruption and Suspension

Implemented **signal handling** for live process control.

**Key bindings:**

* **Ctrl+C** → Terminates the running process (without closing the shell).
* **Ctrl+Z** → Suspends (pauses) the running process and returns to the shell prompt.

A confirmation message is displayed when a process is stopped.

---

### 10. Searchable Shell History

Implemented a **persistent command history system** that saves the last **10,000 commands** to `~/.myterm_history` and allows interactive search.

**Features:**

* **history** → Displays the most recent **1,000 commands**.
* **Ctrl+R** → Opens an interactive “Enter search term” prompt to search past commands.
* Finds **exact** or **substring-based matches**.
* Prints:

  ```
  No match for search term in history
  ```

  when no relevant entries are found.

---

### 11. Auto-Complete for File Names

Implemented **auto-complete** for filenames in the current directory using the **Tab** key.

**Usage**
* Type a few characters of a filename and press Tab.

**Behavior:**
* If one file matches → Automatically completes the filename.
* If multiple files match → Displays a numbered list (e.g., `1. abc.txt  2. abcd.txt`).
* The user can press the corresponding number to choose.
* Works for both absolute and relative paths (e.g., `./myprog de` → `./myprog def.txt`).

### Extra Feature — Paste Command Support

The terminal supports pasting commands directly from the clipboard (e.g., using right-click or Ctrl+V).
This allows users to quickly paste and execute multi-line or complex commands seamlessly inside the terminal.

---



## Folder Structure

```
myterm_project/
├── include/               # Header files
│   ├── line_edit.h
│   ├── history.h
│   ├── multiwatch.h
│   ├── shell_tab.h
│   └── cmd_exec.h
│   └── autocomplete.h
├── src/                   # Source files
│   ├── main.c             # Entry point and X11 event loop
│   ├── cmd_exec.c         # Command execution logic
│   ├── history.c          # Persistent command history
│   ├── multiwatch.c       # Parallel command runner
│   ├── line_edit.c        # Line editor (input management)
│   ├── shell_tab.c        # Tab management system
│   └── autocomplete.c     # Command and file name completion
├── build/                 # Object files (generated after compilation)
├── Makefile               # Build configuration
└── README.md              # Project documentation
└── DESIGNDOC              # Detailed design explanation of implementation
```

---

## Author

**Name:** Rounak Neogy  
**Roll Number:** 25CS60R55  
**Course:** CS69201 - Computing Lab  
**Project:** MyTerm - Custom X11 Shell

---

---

## Future Enhancements

* Mouse-based scrolling and selection.
* Configurable keybindings and theme options.
* Integrated terminal logs for debugging sessions.