# Shell scripts

dream's shell can run scripts: text files of commands with variables, loops and
decisions. It's a small cousin of the Unix shell (`sh`/`bash`), so what you learn here
mostly carries over.

## A first script

Open notes (`notes hello.sh &`) and type:

```sh
#!/bin/sh
# hello.sh: say hello a few times
name=$1
if [ -z "$name" ]; then
    name=world
fi
for i in 1 2 3; do
    echo "hello $name ($i)"
done
```

Save it, then in the terminal:

```
./hello.sh            hello world (1) ... (3)
./hello.sh Lynwood    hello Lynwood (1) ... (3)
sh hello.sh           the same: sh (or source) runs any file as a script
```

A file whose first line starts with `#!` runs like a program. Put it in `/bin` and
you can call it by name from anywhere: `cp hello.sh /bin/greetme`, then `greetme`.
(Built-in commands win over files in `/bin`, so pick a name `help` doesn't list.)

Everything also works on one line at the prompt, with `;` between statements:

```
for i in 1 2 3; do echo $i; done
```

## Variables

```sh
name=dream                 # no spaces around =
greeting="hello there"     # quotes keep the space
echo $name ${name}os       # ${...} when letters follow
echo "$greeting, $name"    # filled in inside "double quotes"
echo '$name'               # ...but not inside 'single quotes'
echo \$name                # or after a backslash
```

`set` lists every variable, `unset name` forgets one, `read name` reads a line from
the keyboard into one (`read first rest` splits it into words; the last gets the rest).

Some are always there:

| | |
|---|---|
| `$1` ... `$9` | the script's arguments. `$0` is its name |
| `$#` | how many arguments |
| `$@` | all of them (`shift` drops `$1` and moves the rest up) |
| `$?` | the status of the last command (0 = it worked) |
| `$HOME`, `$PWD`, `$HOSTNAME` | home folder, current folder, the computer's name |
| `$RANDOM` | a random number from 0 to 32767 |

## Sums: `$(( ))`

```sh
n=5
echo $((n * 2 + 1))        # 11. inside $(( )) the $ before names is optional
n=$((n - 1))
echo $((17 % 5)) $((7 / 2)) $((n > 3))    # 2 3 1 (whole numbers; comparisons give 1 or 0)
```

## A command's output: `$( )`

```sh
count=$(ls /bin | wc -w)
echo "there are $count programs"
echo "today is $(date)"
```

## Did it work? Status, `&&` and `||`

Every command finishes with a status: 0 if it worked, something else if not. Programs
choose theirs (`return 3` from `main`, or `exit 3` in a script); built-in commands fail
when they print an error.

```sh
mkdir projects && echo made it       # only if mkdir worked
cd nowhere || echo "no such folder"   # only if cd failed
false; echo $?                        # 1
```

## test, or [ ]

`test` checks something and fails quietly if it isn't so. `[ ... ]` is the same thing
(the spaces inside the brackets matter):

| | true when |
|---|---|
| `[ -f path ]` / `[ -d path ]` / `[ -e path ]` | it's a file / a folder / either |
| `[ -z "$s" ]` / `[ -n "$s" ]` | the string is empty / isn't |
| `[ "$a" = "$b" ]` / `[ "$a" != "$b" ]` | the strings are the same / different |
| `[ $n -eq 3 ]` | numbers: `-eq -ne -lt -le -gt -ge` |
| `[ ! -f path ]` | `!` turns any of them around |

Quote variables that might be empty or have spaces: `[ "$x" = yes ]`.

## if, while, until, for

```sh
if [ -f ~/todo.txt ]; then
    cat ~/todo.txt
elif [ -d ~/notes ]; then
    ls ~/notes
else
    echo nothing yet
fi

n=3
while [ $n -gt 0 ]; do     # until [ ... ] loops while it's NOT so
    echo $n
    n=$((n - 1))
done

for f in a b c; do echo $f; done
for arg; do echo $arg; done      # no "in": the script's arguments
```

`break` leaves a loop, `continue` skips to the next round, and `exit 2` ends the
script with status 2. **ctrl+c** stops a script or loop that's running.

## Comments

Anything after a `#` at the start of a word is ignored, to the end of the line.

## Your startup script

If `~/.dreamrc` exists, the shell runs it once when dream starts: a good place for
variables or a greeting.

```sh
echo "welcome back. $(cat ~/todo.txt | wc -l) things to do."
```

## Scripts that come with dream

They're in `/bin`, and they're meant to be read: `notes /bin/todo &`.

| | |
|---|---|
| `todo`, `todo add <words>`, `todo clear` | a to-do list in `~/todo.txt` |
| `countdown [seconds] [message]` | counts down, then shows a notification |
| `greet` | asks your name the first time, and remembers it |

## What's missing (compared with bash)

Functions, `case`, arrays, `<` input redirection, `while read` over a file, and
`name=value command` (a variable for just one command). The engine is
`kernel/shell/script.c`, about 700 lines with comments, if you'd like to add one.
