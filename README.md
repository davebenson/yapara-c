# yapara

Yet another parallelizer. Reads command-lines, one per line, and runs
them concurrently — roughly the job GNU parallel does, built on
[libuv](https://libuv.org/).

Two things make it different from a shell loop with `&`:

- **Simple lines are not handed to a shell.** `yapara` understands
  enough of shell syntax itself — quoting, `$VAR`, `VAR=value`,
  redirection — to spawn most jobs directly. For short-lived jobs, an
  extra `sh -c` per job can cost more than the job. Lines that need a
  real shell are recognised and given one.
- **Presentation is a plug-in, and separate from recording.** One run
  has one UI — tagged lines, or a full-screen two-pane browser — plus
  any number of recorders writing it to disk. Watching a run and
  keeping a record of it are not an either/or.

## Building

Needs a C compiler, libuv, and pkg-config.

```sh
./configure
make
make check
sudo make install
```

From a git checkout, generate `configure` first:

```sh
./autogen.sh
```

That needs autoconf and automake. It also needs `pkg.m4`, which
pkg-config installs — on Homebrew it lands outside automake's search
path, so `autogen.sh` looks in the usual places and sets `ACLOCAL_PATH`
itself.

## Usage

```sh
yapara --input=JOBS [OPTIONS]
```

`--input=-` reads the job list from standard input.

```sh
$ cat jobs.txt
convert a.png a.jpg
convert b.png b.jpg

# comments and blank lines are skipped
gzip -9 big.log

$ yapara --input=jobs.txt --max=4
```

`yapara` exits 0 if every job exited 0, and 1 otherwise.

### Options

| Option | Meaning |
| --- | --- |
| `--input=FILENAME` | Job list, one command-line per line. `-` for stdin. Required. |
| `--max=N` | How many jobs to run at once. Default 10. |
| `--ui=NAME` | Which presentation to use. Default `plain`. `--list-uis` names them, `--help-ui=NAME` explains one. |
| `--recorder=NAME` | Also record the run, alongside the UI. Repeatable. `--list-recorders` names them. |
| `--out-dir=DIR` | Where anything that writes files should put them. Created if missing, parents included. |
| `--colorize[=WHEN]`, `-c` | Tint output by job. `auto` (the default: only when stdout is a terminal), `always`, `never`. |
| `--index-width=N` | Pad job indices to at least N digits. |
| `--index-zero-pad` | Pad indices with zeroes rather than spaces. |
| `--index-hex` | Print indices in hexadecimal. |
| `--ui-option=KEY=VALUE` | Pass a setting through to the UI. Repeatable; the last use of a key wins. |

Job indices count from 0, in the order jobs start.

A run has **one UI and any number of recorders**, so watching a run and
writing it down are not an either/or:

```sh
yapara --input=jobs.txt --ui=two-pane --recorder=jobs --out-dir=results
```

## Job-file syntax

Each line is a command. `yapara` parses these itself:

```sh
program arg1 arg2                # words, split on whitespace
program 'single quoted'          # no expansion inside
program "double $QUOTED"         # $VAR expands
program back\ slashed            # escapes
program $HOME ${HOME}            # from yapara's own environment
VAR=value program                # leading assignments go into the child's env
program < in.txt > out.txt       # redirection
program >> log.txt 2> err.txt    # appending, and per-fd
program > log.txt 2>&1           # merging stderr into stdout
program 3< extra.dat             # any descriptor, not just 0/1/2
```

Two details worth knowing, both matching what `sh` does:

- An **unquoted** `$VAR` is split into separate arguments on whitespace;
  a quoted `"$VAR"` is one argument, even if empty.
- An assignment on a line is **not** visible to expansions on that same
  line: `FOO=2 echo $FOO` echoes the old `FOO`.

### When a real shell is used

Anything else — pipelines, `&&`, `;`, globs, backticks, `$(...)`, `~` —
is not guessed at. The whole line is handed to `sh -c` verbatim:

```sh
gzip -9 a.log && rm a.log        # sh -c
find . -name '*.o' | xargs rm    # sh -c
ls *.png                         # sh -c (glob)
```

One case is subtler. `program > log 2>&1` is translated directly, but
`program 2>&1 > log` is given to `sh`. In a shell the second form
points stderr at the *old* stdout, and yapara's process description
records only where each descriptor ends up — so rather than quietly
sending both streams to `log`, it defers to something that gets it
right.

A malformed line — an unterminated quote, a redirection with no
filename — is an error rather than a shell's problem, since yapara can
point at the column.

Unless a line redirects its own stdin, jobs get `/dev/null` there.
Concurrent jobs sharing a terminal would only fight over it.

## User interfaces

### `plain` (default)

Output as it arrives, undecorated. Failures are named on stderr.

```
$ yapara --input=jobs.txt
job one output
job two output
sh -c 'exit 4': exited with status 4
1 of 6 jobs failed (7 lines).
```

### `prefix`

Every line tagged with its job index and stream — `O` for stdout, `E`
for stderr. Notes from yapara itself use `!` and go to stderr, so
`2>/dev/null` leaves only job output.

```
$ yapara --input=jobs.txt --ui=prefix --max=1
0O: alpha
1O: out-1
1E: err-1
2O: merged
4!: exited with status 4
```

Child stderr goes to *yapara's* stdout, not its stderr: the tag already
says which stream it was, and keeping one stream means the ordering
survives being piped somewhere.

Combines with the index options:

```
$ yapara --input=jobs.txt --ui=prefix --index-width=6 --index-zero-pad
000000O: alpha
000001O: out-1
```

### `two-pane`

Full screen: a job list on top, the selected job's output below. Needs
a terminal.

```
 3 running, 12 done, 1 failed
> 0003  running  convert c.png c.jpg
  0004  running  convert d.png d.jpg
  0001  exit 1   convert a.png a.jpg
-- job 0003 -----------------------------------
converting c.png
done
```

| key | |
| --- | --- |
| `Up`/`Down`, `k`/`j` | select a job |
| `PgUp`/`PgDn` | scroll the output |
| `Home`/`End` | oldest / newest output |
| `[`/`]` | move the divider |
| `q`, `Ctrl-C` | quit |

Each job's output goes into a fixed-size circular buffer, so a job that
prints for an hour costs the same as one that prints a line. Finished
jobs stay listed and stay readable — the job you want to look at is
usually the one that just failed. The run does not exit when the last
job does; it waits for you to quit.

`--ui-option` settings: `buffer-size`, `max-lines`, `keep-jobs`,
`job-rows`.

### `passthrough`

Gets out of the way: the children inherit yapara's own stdout and
stderr and write straight to them. Nothing is copied through the
process, and interleaving is whatever the OS does — much like `cmd &`
in a shell.

This is worth more than it sounds. Putting a pipe on a child's stdout
makes its libc switch from line buffering to 4kB block buffering, so a
chatty job stops emitting whole lines and starts emitting late blocks
that splice into other jobs' output mid-line. Leaving the terminal in
place keeps every child line-buffered. The trade is that nothing knows
which job wrote what, so there is nothing to tag or colour.

## Recorders

A recorder observes a run without presenting it, and any number can run
alongside the chosen UI:

```sh
yapara --input=jobs.txt --ui=two-pane --recorder=jobs --out-dir=results
```

### `jobs`

Four files per job under `--out-dir`.

```
$ ls results
0000-end.json  0000-start.json  0000.stdout
0001-end.json  0001-start.json  0001.stderr  0001.stdout
```

`.stdout` and `.stderr` hold exactly the bytes the child wrote — no
line splitting, no newline translation, no assumption it was text.
They are created on first write, so a job that never touched stderr
leaves no `.stderr` file; the byte counts in `-end.json` say what to
expect, so absence is unambiguous.

`-end.json` has a fixed shape, with `exit_code` and `signal` both
always present and `null` for whichever does not apply:

```json
{
  "index": 3,
  "pid": 39646,
  "cmdline": "sh -c 'kill -TERM $$'",
  "started_micros": 1788402255623319,
  "ended_micros": 1788402255626175,
  "elapsed_micros": 2856,
  "status": "killed",
  "exit_code": null,
  "signal": 15,
  "stdout_bytes": 0,
  "stderr_bytes": 0
}
```

Filenames always zero-pad, whatever `--index-zero-pad` says, so they
sort lexically — a filename with spaces in it is nobody's friend.

## Writing a UI

A UI is a `YcUIFuncs` vtable plus optional instance state. The
framework assigns job indices, puts stdout/stderr on pipes, reads them,
reassembles lines across read boundaries, and tracks which jobs are
running. See `src/yc-ui.h`, and `src/yc-ui-prefix.c` for about the
smallest useful example.

```c
static void
my_job_line (YcUI *ui, YcUIJob *job, int child_fd,
             const char *line, size_t len, uint64_t micros)
{
  char index[YC_UI_INDEX_BUF_SIZE];
  printf ("%s: %s\n", yc_ui_job_index_string (ui, job, index, sizeof index),
          line);
}

const YcUIFuncs my_ui = {
  "mine", "one line per line", 0, 0,
  NULL,                 /* init */
  NULL,                 /* job_started */
  NULL,                 /* job_output: raw bytes, if you want them */
  my_job_line,
  NULL, NULL, NULL      /* job_ended, all_done, destroy */
};
```

Register it in `register_builtins_once()` in `src/yc-ui.c` and it shows
up in `--ui` and in `--help` automatically.

Three things to know:

- Setting `job_output` or `job_line` is what makes the framework
  capture output at all. A UI that sets neither leaves the child's
  stdout inherited. There is no separate "please capture" flag to get
  out of step with.
- `job_output` gets bytes exactly as read, with a timestamp;
  `job_line` gets them stitched into lines with the newline (and any
  `\r` before it) removed. They are independent, and both fire if you
  set both.
- Anything hung off `job->ui_data` must be freed in `job_ended`.

If what you are writing only *observes* a run — writing it to disk,
shipping it somewhere — write a **recorder** instead, against the much
smaller interface in `src/yc-recorder.h`. A recorder gets told that a
job started, that bytes arrived and that it ended; it does not subclass
the job, does not see `ui->jobs`, and never touches the terminal. That
is what lets any number of them run alongside the UI. `yc-recorder-jobs.c`
is the worked example.

If your UI writes files, use `--out-dir` via
`yc_ui_options_ensure_out_dir()` rather than inventing another option.
And if it mixes output on stdout with diagnostics on stderr, `fflush`
stdout before the diagnostic — stdout is block-buffered when it is not
a terminal and the complaint will otherwise overtake what it is about.

## Tests

```sh
make check
```

Five programs:

| | |
| --- | --- |
| `test-shell` | the job-line parser, in isolation |
| `test-child` | process supervision — really spawns processes |
| `test-ui` | the UI framework, the bundled UIs, and the recorders |
| `test-term` | the terminal layer: frames, clipping, key decoding |
| `test-circular-buffer` | the bounded per-job output buffer |

`test-child` and `test-ui` have watchdog alarms, because several of the
things they check fail by hanging rather than by giving a wrong answer.
`test-term` runs entirely headless — `yc_term_new_headless()` renders
frames into memory and takes fed keystrokes — so the TUI's layout and
key handling are asserted byte-for-byte rather than eyeballed.

## Layout

| | |
| --- | --- |
| `src/yc-child.c` | spawning and supervising children; the concurrency limit |
| `src/yc-shell.c` | one line of shell into a process description |
| `src/yc-ui.c` | plug-in framework: indices, capture, line reassembly, recorders |
| `src/yc-ui-*.c` | the bundled UIs |
| `src/yc-recorder-*.c` | the bundled recorders |
| `src/yc-term.c` | raw mode, key decoding and a diffed frame buffer |
| `src/yc-circular-buffer.c` | bounded line storage, for the two-pane UI |
| `src/yapara-main.c` | command-line handling and the job-file reader |
| `src/yc-buffer.c`, `yc-cmdline.c`, `yc-alloc.c`, `yc-common.c` | support code |

## License

[0BSD](https://spdx.org/licenses/0BSD.html) — see `COPYING`.

This is the BSD family's most permissive variant: use it, change it,
ship it, with or without attribution. There is no requirement to
reproduce the copyright notice, so vendoring a file into another
project needs nothing done about paperwork.

Copyright (C) 1999-2026 Dave Benson.
