#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>

#define MAX_CMD_LEN 4096
#define VERSION "0.1.0"

typedef struct {
	char** items;
	size_t cap;
	size_t len;
} StrArr;

#define DA_INIT_CAP 4
#define da_append(arr,item)                                         \
	do {                                                            \
		if((arr)->len>=(arr)->cap) {                                \
			if((arr)->cap) (arr)->cap*=2;                           \
			else (arr)->cap=DA_INIT_CAP;                            \
			void* items=malloc((arr)->cap*sizeof(item));            \
			if((arr)->items) {                                      \
				memcpy(items,(arr)->items,(arr)->len*sizeof(item)); \
				free((arr)->items);                                 \
			}                                                       \
			(arr)->items=items;                                     \
		}                                                           \
		(arr)->items[(arr)->len++]=item;                            \
	} while(0)

size_t trim(char** str) {
	size_t len=strnlen(*str,MAX_CMD_LEN);
	while(len && isspace((*str)[len-1])) {
		len--;
	}
	while(len && (*str)[0] && isspace((*str)[0])) {
		(*str)++;
		len--;
	}
	if(len<MAX_CMD_LEN) (*str)[len]='\0';
	return len;
}

void parse_args(StrArr* cmd,char* command) {
	size_t len=trim(&command);
	size_t tlen=0;
	for(size_t i=0; i<len; i++) {
		if(isspace(command[i])) {
			if(tlen) {
				command[i]='\0';
				da_append(cmd,command+i-tlen);
				tlen=0;
			}
		} else {
			tlen++;
		}
	}
	if(tlen) da_append(cmd,command+len-tlen);
}

void remove_dir(char* res,char* path) {
	char* shortpath=path;
	for(size_t i=0; path && path[i]; i++) {
		if(path[i]=='/') {
			shortpath=path+i+1;
		}
	}
	strcpy(res,shortpath);
}

typedef struct termios Termios;
Termios initial_state={0};
int keys_fd=0;
size_t term_width=80;

void getsize(int _sig) {
	(void)_sig;
	struct winsize win;
	ioctl(0,TIOCGWINSZ,&win);
	term_width=win.ws_col;
}

void readline(char* prompt,char* command,StrArr history) {
	size_t idx=0;
	size_t curlen=0;
	size_t hist_idx=history.len;
	bool edited=false;
	getsize(0);
	size_t prompt_len=strlen(prompt)%term_width;
	tcgetattr(keys_fd,&initial_state);
	Termios raw=initial_state;
	raw.c_lflag&=~(ISIG|ICANON|ECHO);
	raw.c_cc[VMIN]=1;
	raw.c_cc[VTIME]=0;
	printf("%s",prompt);
	command[0]='\0';
	tcsetattr(keys_fd,TCSAFLUSH,&raw);
	unsigned char ch=0;
	while(ch!='\n') {
		ch=getchar();
got_char:
		if(!ch) break;
		switch(ch) {
			case '\x1b':
parse_esc:
				ch=getchar();
				switch(ch) {
					case '[':
						goto parse_esc;
					case 'A':
prev_hist:
						if(hist_idx>0) {
							if(edited) {
								// TODO search backwards through history for a match of current command
							}
							if(history.items[--hist_idx]) {
								memcpy(command,history.items[hist_idx],MAX_CMD_LEN);
								curlen=strlen(command);
								idx=curlen;
							} else {
								curlen=0;
								idx=0;
							}
							edited=false;
							printf("\r\x1b[K%s%s",prompt,command);
						}
						break;
					case 'B':
next_hist:
						if(hist_idx<history.len) {
							if(edited) {
								// TODO search backwards through history for a match of current command
							}
							if(history.items[++hist_idx]) {
								memcpy(command,history.items[hist_idx],MAX_CMD_LEN);
								curlen=strlen(command);
								idx=curlen;
							} else {
								curlen=0;
								idx=0;
								command[0]='\0';
							}
							edited=false;
							printf("\r\x1b[K%s%s",prompt,command);
						}
						break;
					case 'C':
move_right:
						if(idx<curlen) {
							printf("%c",command[idx]);
							idx++;
						}
						break;
					case 'D':
move_left:
						if(idx>0) {
							printf("\b");
							idx--;
						}
						break;
					case '3': // Delete
						ch=getchar();
						if(ch!='~') goto got_char;
delete_char:
						if(idx<curlen) {
							for(size_t i=idx; i+1<curlen; i++) {
								command[i]=command[i+1];
							}
							curlen--;
							printf("\x1b""7%.*s \x1b""8",(int)(curlen-idx),command+idx);
						}
						break;
					default:
						goto got_char;
				}
				break;
			case 'A'-'@':
				if(idx+prompt_len>term_width) printf("\x1b[%zuA",(idx+prompt_len)/term_width);
				printf("\r\x1b[%zuC",prompt_len);
				idx=0;
				break;
			case 'E'-'@':
				if(curlen+prompt_len>term_width) printf("\x1b[%zuB",(curlen-idx+prompt_len)/term_width);
				printf("\r\x1b[%zuC",(curlen+prompt_len)%term_width);
				idx=curlen;
				break;
			case 'D'-'@':
				if(curlen>0) goto delete_char;
				// FIXME very hacky way of quitting :)
				sprintf(command,"exit");
				curlen=4;
				ch='\n';
				break;
			case 'C'-'@':
				printf("\r%*c\r%s",(int)(prompt_len+curlen),' ',prompt);
				curlen=0;
				break;
			case 'F'-'@':
				goto move_right;
			case 'B'-'@':
				goto move_left;
			case 'H'-'@':
			case 127: // Backspace
				if(idx>0) {
					for(size_t i=idx-1; i<curlen && i+1<MAX_CMD_LEN; i++) {
						command[i]=command[i+1];
					}
					curlen--;
					printf("\x1b[D\x1b""7%.*s \x1b""8",(int)(curlen-idx+1),command+idx-1);
					idx--;
				}
				break;
			case 'N'-'@':
				goto next_hist;
				break;
			case 'P'-'@':
				goto prev_hist;
				break;
			default:
				if(ch<' ') continue;
				if(idx+1>=MAX_CMD_LEN) continue;
				if(idx<curlen) {
					printf(" %.*s\b",(int)(curlen-idx),command+idx);
					for(size_t i=curlen; i>idx; i--) {
						command[i]=command[i-1];
						printf("\b");
					}
				}
				curlen++;
				if(curlen>=MAX_CMD_LEN) curlen=MAX_CMD_LEN-1;
				printf("%c",ch);
				fflush(stdout);
				command[idx]=ch;
				edited=true;
				idx++;
		}
		if(curlen<MAX_CMD_LEN) command[curlen]='\0';
		else curlen--;
	}
	for(size_t i=(prompt_len+curlen-idx)/term_width+1; i>0; i--) printf("\r\n");
	tcsetattr(keys_fd,TCSAFLUSH,&initial_state);
	command[MAX_CMD_LEN-1]='\0';
}

void add_history(StrArr cmd,StrArr* history) {
	if(cmd.len==0) return;
	char command[MAX_CMD_LEN];
	sprintf(command,"%s",cmd.items[0]);
	size_t tlen=strlen(cmd.items[0]);
	for(size_t i=1; i<cmd.len; i++) {
		size_t arglen=strlen(cmd.items[i]);
		if(arglen==0) continue;
		sprintf(command+tlen," %s",cmd.items[i]);
		tlen+=arglen+1;
	}
	if(history->len>0 && strcmp(command,history->items[history->len-1])==0) return;
	char* copy=malloc(MAX_CMD_LEN);
	memcpy(copy,command,strlen(command));
	da_append(history,copy);
}

void version(char* program,FILE* fd) {
	fprintf(fd,"%s (Abyss Shell) version %s\n",program,VERSION);
}

void help(char* program,FILE* fd) {
	version(program,fd);
	fprintf(fd,"\n");
	fprintf(fd,"List of builtin commands:\n");
	fprintf(fd,"    exit           Close the shell\n");
	fprintf(fd,"    hax            Attempt to close STDIN (bug testing)\n");
	fprintf(fd,"    cd directory   Change CWD to directory\n");
	fprintf(fd,"    version        Prints the version of the shell in a single line\n");
	fprintf(fd,"    help           Print this help\n");
}

int main(int argc,char** argv) {
	signal(SIGWINCH,getsize);
	char* pname=argv[0];
	remove_dir(pname,pname);
	if(strlen(pname)==0 || argc<1) {
		pname="(abysh)";
		fprintf(stderr,"%s: Warning: weird environment\n",pname);
	}
	StrArr history={0};
	char command[MAX_CMD_LEN];
	char cwd[PATH_MAX];
	char promptpath[PATH_MAX];
	char prompt[PATH_MAX*2];
	StrArr cmd={0};
	while(1) {
		getcwd(cwd,PATH_MAX);
		remove_dir(promptpath,cwd);
		if(promptpath[0]=='\0') strcpy(promptpath,cwd);
		snprintf(prompt,sizeof(prompt),"%s %s > ",pname,promptpath);
		readline(prompt,command,history);
		memset(&cmd,0,sizeof(cmd));
		parse_args(&cmd,command);
		if(cmd.len) {
			add_history(cmd,&history);
			if(strcmp(cmd.items[0],"exit")==0) return 0;
			if(strcmp(cmd.items[0],"hax")==0) {
				printf("breaking your code >:)\n");
				fclose(stdin);
			}
			if(strcmp(cmd.items[0],"cd")==0) {
				char* newdir;
				if(cmd.len==1) {
					newdir=getenv("HOME");
				} else {
					newdir=cmd.items[1];
				}
				int res=chdir(newdir);
				if(res<0) {
					fprintf(stderr,"%s: cd %s: %s\n",pname,newdir,strerror(errno));
				}
				continue;
			}
			if(strcmp(cmd.items[0],"version")==0) {
				version(pname,stdout);
				continue;
			}
			if(strcmp(cmd.items[0],"help")==0) {
				help(pname,stdout);
				continue;
			}
			da_append(&cmd,NULL);
			pid_t pid=fork();
			if(pid==0) {
				int res=execvp(cmd.items[0],cmd.items);
				if(res<0) {
					fprintf(stderr,"Unknown command: %s\n",cmd.items[0]);
					return 127;
				}
				fprintf(stderr,"%s: internal error\n",pname);
				return 1;
			} else {
				int status;
				waitpid(pid,&status,0);
				printf("ret: %d\n",WEXITSTATUS(status));
			}
		}
	}
}
