#pragma once
#include "kernel.h"

/* Every command has the same shape: void cmd_NAME(int argc, char **argv) */
#define SH_CMD(name) void cmd_##name(int argc, char **argv)

/* Shell state commands can use */
int  sh_cwd(void);
void sh_set_cwd(int node);
bool sh_is_tty(void);
int  sh_wait_key(void);         /* block until a key; blinks the cursor meanwhile */
int  sh_poll_key(void);         /* a key if one is waiting, else -1; keeps the desktop alive */
bool sh_at_prompt(void);        /* waiting for a command line (not inside a command) */            /* false when output goes into a pipe or a file */
const char *sh_hostname(void);
void sh_print_history(void);
bool sh_parse_num(const char *s, uint64_t *out);
void sh_perror(const char *cmd, const char *what, int fs_err);
int  sh_install_programs(void);   /* copy the built-in user programs into /bin */

/* Input for filters like cat/grep/wc: the file named by argv[i] if there is
   one, otherwise whatever was piped in. Prints an error and returns NULL if neither. */
const char *sh_input(int argc, char **argv, int i, size_t *len);

/* shell/cmd_fs.c */
SH_CMD(ls); SH_CMD(cd); SH_CMD(pwd); SH_CMD(mkdir); SH_CMD(touch); SH_CMD(cat);
SH_CMD(rm); SH_CMD(rmdir); SH_CMD(mv); SH_CMD(cp); SH_CMD(tree);
SH_CMD(wc); SH_CMD(head); SH_CMD(tail); SH_CMD(grep);

/* shell/cmd_disk.c */
SH_CMD(lsblk); SH_CMD(blkread); SH_CMD(blkwrite); SH_CMD(hexdump); SH_CMD(sync); SH_CMD(mkfs);

/* apps/edit.c */
SH_CMD(edit); SH_CMD(paint);

/* gui/wm.c */
SH_CMD(startx); SH_CMD(exit); SH_CMD(files); SH_CMD(lock); SH_CMD(notify); SH_CMD(open);

/* shell/cmd_sys.c */
SH_CMD(uname); SH_CMD(whoami); SH_CMD(hostname); SH_CMD(date); SH_CMD(free);
SH_CMD(dmesg); SH_CMD(history); SH_CMD(sleep); SH_CMD(lscpu); SH_CMD(memtest); SH_CMD(ps);
